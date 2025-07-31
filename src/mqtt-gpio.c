// SPDX-License-Identifier: OSL-3.0
/*
 * Copyright (C) 2023  Trevor Woerner <twoerner@gmail.com>
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <gpiod.h>
#include <mosquitto.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "config.h"

#define NOTU __attribute__((unused))
#define DEFAULT_CONFIG_FILE "/mqtt-gpio.conf"

typedef struct {
	char *gpioID_p;
	char *chipStr_p;
	struct gpiod_chip *chip_p;
	int pin;
	struct gpiod_line *line_p;
	struct gpiod_line_request_config config;
} GPIOinfo_t;

typedef struct {
	char *cmdID_p;
	char *cmdStr_p;
	bool oneshot;
	bool valid;
	pid_t pid;
	bool running;
} CMDinfo_t;

typedef struct {
	char *topicStr_p;
	char *linkID_p;
	int qos;
	bool inv;
} SUBinfo_t;

static char *defaultConfigFileName_pG = NULL;
static char *userConfigFile_pG = NULL;
static int verbose_G = 0;
static GPIOinfo_t *gpioInfo_pG = NULL;
static int gpioInfoCnt_G = 0;
static SUBinfo_t *subInfo_pG = NULL;
static int subInfoCnt_G = 0;
static CMDinfo_t *cmdInfo_pG = NULL;
static int cmdInfoCnt_G = 0;
static char *mqttServer_pG = NULL;
static int mqttServerPort_G = 0;
static struct mosquitto *mosq_pG = NULL;

static void usage (char *pgm_p);
static void parse_cmdline (int argc, char *argv[]);
static void set_default_config_filename (void);
static void process_config_file (void);
static void init_SUBinfo (void);
static void init_GPIOinfo (void);
static void init_CMDinfo (void);
static void init_mosquitto (void);
static void cleanup (void);
static void connect_callback (struct mosquitto *mosq_p, void *userdata_p, int result);
static void process_message (struct mosquitto *mosq_p, void *userdata_p, const struct mosquitto_message *msg_p);

int
main (int argc, char *argv[])
{
	atexit(cleanup);

	set_default_config_filename();
	parse_cmdline(argc,argv);
	process_config_file();
	init_GPIOinfo();
	init_CMDinfo();
	init_SUBinfo();
	init_mosquitto();
	mosquitto_loop_forever(mosq_pG, -1, 1);

	return EXIT_SUCCESS;
}

static void
usage (char *pgm_p)
{
	printf("usage: %s [OPTIONS]\n", pgm_p);
	printf("  where <OPTIONS> are:\n");
	printf("    -h | --help        Print help options to terminal and exit successfully\n");
	printf("    -v | --version     Show program version information and exit successfully\n");
	printf("    -V | --verbose     Run program verbosely, use multiple for more verbosity\n");
	printf("    -c | --config <f>  Use <f> for config instead of default (%s)\n",
			defaultConfigFileName_pG);
}

static void
parse_cmdline (int argc, char *argv[])
{
	int c;
	struct option longOpts[] = {
		{"help",    no_argument,       NULL, 'h'},
		{"version", no_argument,       NULL, 'v'},
		{"verbose", no_argument,       NULL, 'V'},
		{"config",  required_argument, NULL, 'c'},
		{NULL, 0, NULL, 0},
	};

	while (1) {
		c = getopt_long(argc, argv, "hvVc:", longOpts, NULL);
		if (c == -1)
			break;
		switch (c) {
			case 'h':
				usage(argv[0]);
				exit(EXIT_SUCCESS);

			case 'v':
				printf("%s\n", PACKAGE_STRING);
				exit(EXIT_SUCCESS);

			case 'V':
				++verbose_G;
				break;

			case 'c':
				free(defaultConfigFileName_pG);
				defaultConfigFileName_pG = NULL;
				userConfigFile_pG = optarg;
				break;

			default:
				if (verbose_G > 0)
					printf("getopt() issue: %c (0x%02x)\n", c, c);
				exit(EXIT_FAILURE);
		}
	}

	if (optind < argc) {
		printf("extra cmdline args\n\n");
		exit(EXIT_FAILURE);
	}
}

static void
set_default_config_filename (void)
{
	size_t len;

	// set default config file/location based on ./configure
	len = strlen(ETCPKGDIR) + strlen(DEFAULT_CONFIG_FILE);
	defaultConfigFileName_pG = (char*)malloc(len + 1);
	if (defaultConfigFileName_pG == NULL) {
		perror("malloc()");
		exit(EXIT_FAILURE);
	}
	memset(defaultConfigFileName_pG, 0, len+1);
	defaultConfigFileName_pG = strcat(defaultConfigFileName_pG, ETCPKGDIR);
	defaultConfigFileName_pG = strcat(defaultConfigFileName_pG, DEFAULT_CONFIG_FILE);
	userConfigFile_pG = defaultConfigFileName_pG;
}

static void
process_config_file (void)
{
	FILE *stream_p;
	char *line_p = NULL;
	size_t len = 0;
	ssize_t nread;
	const char *delim_p = " \t\n";
	char *token_p;
	unsigned lineCnt;

	if (userConfigFile_pG == NULL) {
		printf("no config file specified\n");
		exit(EXIT_FAILURE);
	}

	stream_p = fopen(userConfigFile_pG, "r");
	if (stream_p == NULL) {
		perror("fopen()");
		printf("%s\n", userConfigFile_pG);
		exit(EXIT_FAILURE);
	}

	lineCnt = 0;
	while ((nread = getline(&line_p, &len, stream_p)) != -1) {
		++lineCnt;

		if (verbose_G > 1)
			printf("config[%03d]: %s", lineCnt, line_p);

		// skip blank lines and lines starting with '#'
		if (line_p[0] == '#') {
			if (verbose_G > 1)
				printf(" skipping comment\n");
			continue;
		}
		if (line_p[0] == '\n') {
			if (verbose_G > 1)
				printf(" skipping empty line\n");
			continue;
		}

		token_p = strtok(line_p, delim_p);
		if (token_p == NULL) {
			printf("   invalid config line #%d: no CMD\n", lineCnt);
			continue;
		}

		// MQTT
		if (strcmp(token_p, "MQTT") == 0) {
			if (verbose_G)
				printf("found an MQTT\n");

			// server DNS/IP
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: MQTT server DNS/IP expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G)
				printf("   MQTT server DNS/IP: %s\n", token_p);
			mqttServer_pG = strdup(token_p);
			if (mqttServer_pG == NULL) {
				perror("strdup(MQTT server)");
				exit(EXIT_FAILURE);
			}

			// server port
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: MQTT server port expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			mqttServerPort_G = atoi(token_p);
			if (verbose_G)
				printf("   MQTT port: %d\n", mqttServerPort_G);

			continue;
		}

		// GPIO
		if (strcmp(token_p, "GPIO") == 0) {
			if (verbose_G > 1)
				printf(" found a GPIO (cnt:%u)\n", gpioInfoCnt_G);

			if ((gpioInfoCnt_G+1) == INT_MAX) {
				printf("   no more room in GPIO table, not added\n");
				continue;
			}
			gpioInfo_pG = (GPIOinfo_t*)realloc(gpioInfo_pG,
					((gpioInfoCnt_G+1) * sizeof(GPIOinfo_t)));
			if (gpioInfo_pG == NULL) {
				perror("realloc(GPIO)");
				exit(EXIT_FAILURE);
			}
			memset(&gpioInfo_pG[gpioInfoCnt_G], 0, sizeof(GPIOinfo_t));
			if (verbose_G > 1)
				printf("   realloc(GPIO)'ed\n");

			// gpio name
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: gpio name expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   gpio name: %s\n", token_p);
			gpioInfo_pG[gpioInfoCnt_G].gpioID_p = strdup(token_p);
			if (gpioInfo_pG[gpioInfoCnt_G].gpioID_p == NULL) {
				perror("strdup(gpio name)");
				exit(EXIT_FAILURE);
			}

			// chip
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: chip expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   chip: %s\n", token_p);
			gpioInfo_pG[gpioInfoCnt_G].chipStr_p = strdup(token_p);
			if (gpioInfo_pG[gpioInfoCnt_G].chipStr_p == NULL) {
				perror("strdup(chip)");
				exit(EXIT_FAILURE);
			}

			// pin
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: pin expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   pin: %s\n", token_p);
			gpioInfo_pG[gpioInfoCnt_G].pin = atoi(token_p);

			++gpioInfoCnt_G;
			continue;
		}

		// CMD
		if (strcmp(token_p, "CMD") == 0) {
			if (verbose_G > 1)
				printf(" found a CMD (cnt:%u)\n", cmdInfoCnt_G);

			if ((cmdInfoCnt_G+1) == INT_MAX) {
				printf("  no more room in CMD table, not added\n");
				continue;
			}
			cmdInfo_pG = (CMDinfo_t*)realloc(cmdInfo_pG,
					((cmdInfoCnt_G+1) * sizeof(CMDinfo_t)));
			if (cmdInfo_pG == NULL) {
				perror("realloc(CMD)");
				exit(EXIT_FAILURE);
			}
			memset(&cmdInfo_pG[cmdInfoCnt_G], 0, sizeof(CMDinfo_t));
			if (verbose_G > 1)
				printf("  realloc(CMD)'ed\n");

			// cmdID
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: cmdID expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   cmdID: %s\n", token_p);
			cmdInfo_pG[cmdInfoCnt_G].cmdID_p = strdup(token_p);
			if (cmdInfo_pG[cmdInfoCnt_G].cmdID_p == NULL) {
				perror("strdup(action name)");
				exit(EXIT_FAILURE);
			}

			// cmd to run
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: cmd to run expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   cmd: %s\n", token_p);
			cmdInfo_pG[cmdInfoCnt_G].cmdStr_p = strdup(token_p);
			if (cmdInfo_pG[cmdInfoCnt_G].cmdStr_p == NULL) {
				perror("strdup(cmd str)");
				exit(EXIT_FAILURE);
			}

			// optional specifier: oneshot
			token_p = strtok(NULL, "\n");
			if (token_p == NULL) {
				printf("   CMD line #%d does not include optional 'oneshot'\n", lineCnt);
				cmdInfo_pG[cmdInfoCnt_G].oneshot = false;
			}
			else {
				if (strncmp(token_p, "oneshot", 7) == 0) {
					printf("   CMD line #%d includes optional 'oneshot'\n", lineCnt);
					cmdInfo_pG[cmdInfoCnt_G].oneshot = true;
				}
				else {
					printf("   invalid config line #%d: optional 'oneshot' expected\n", lineCnt);
					exit(EXIT_FAILURE);
				}
			}

			++cmdInfoCnt_G;
			continue;
		}

		// SUB
		if (strcmp(token_p, "SUB") == 0) {
			if (verbose_G > 1)
				printf(" found a SUB (cnt:%u)\n", subInfoCnt_G);

			if ((subInfoCnt_G+1) == INT_MAX) {
				printf("   no more room in SUB table, not added\n");
				continue;
			}
			subInfo_pG = (SUBinfo_t*)realloc(subInfo_pG,
					((subInfoCnt_G+1) * sizeof(SUBinfo_t)));
			if (subInfo_pG == NULL) {
				perror("realloc(SUB)");
				exit(EXIT_FAILURE);
			}
			memset(&subInfo_pG[subInfoCnt_G], 0, sizeof(SUBinfo_t));
			if (verbose_G > 1)
				printf("   realloc(SUB)'ed\n");

			// topic
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: topic expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   topic: %s\n", token_p);
			subInfo_pG[subInfoCnt_G].topicStr_p = strdup(token_p);
			if (subInfo_pG[subInfoCnt_G].topicStr_p == NULL) {
				perror("strdup(topic)");
				exit(EXIT_FAILURE);
			}

			// linkID name
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: linkID name expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   linkID: %s\n", token_p);
			subInfo_pG[subInfoCnt_G].linkID_p = strdup(token_p);
			if (subInfo_pG[subInfoCnt_G].linkID_p == NULL) {
				perror("strdup(linkID)");
				exit(EXIT_FAILURE);
			}

			// qos
			token_p = strtok(NULL, delim_p);
			if (token_p == NULL) {
				printf("   invalid config line #%d: qos expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   qos: %s\n", token_p);
			subInfo_pG[subInfoCnt_G].qos = atoi(token_p);

			// INV [optional]
			token_p = strtok(NULL, delim_p);
			if (token_p != NULL) {
				if (verbose_G > 1)
					printf("   INV: %s\n", token_p);
				if (strncmp(token_p, "INV", 3) == 0)
					subInfo_pG[subInfoCnt_G].inv = true;
			}

			++subInfoCnt_G;
			continue;
		}

		printf("   invalid config line #%d: unknown CMD: %s\n", lineCnt, token_p);
		exit(EXIT_FAILURE);
	}

	free(line_p);
	fclose(stream_p);
}

static void
init_GPIOinfo (void)
{
	int i, ret;

	if (verbose_G > 0)
		printf("number of GPIO items: %d\n", gpioInfoCnt_G);

	if (gpioInfoCnt_G <= 0)
		return;

	for (i=0; i<gpioInfoCnt_G; ++i) {
		if (verbose_G > 0) {
			printf("GPIO[%d]\n", i);
			printf("\tid: %s\n", gpioInfo_pG[i].gpioID_p);
			printf("\tchip: %s\n", gpioInfo_pG[i].chipStr_p);
			printf("\tpin: %d\n", gpioInfo_pG[i].pin);
		}

		// open chip
		gpioInfo_pG[i].chip_p = gpiod_chip_open_lookup(gpioInfo_pG[i].chipStr_p);
		if (gpioInfo_pG[i].chip_p == NULL) {
			printf("can't open gpio device: %s\n", gpioInfo_pG[i].chipStr_p);
			exit(EXIT_FAILURE);
		}

		// get line
		gpioInfo_pG[i].line_p = gpiod_chip_get_line(gpioInfo_pG[i].chip_p, gpioInfo_pG[i].pin);
		if (gpioInfo_pG[i].line_p == NULL) {
			printf("can't get pin: %d\n", gpioInfo_pG[i].pin);
			exit(EXIT_FAILURE);
		}

		// set config (direction)
		gpioInfo_pG[i].config.request_type = GPIOD_LINE_REQUEST_DIRECTION_OUTPUT;
		ret = gpiod_line_request(gpioInfo_pG[i].line_p, &gpioInfo_pG[i].config, 0);
		if (ret != 0) {
			printf("can't set configuration for subscription %d\n", i);
			exit(EXIT_FAILURE);
		}
	}
}

static void
init_CMDinfo (void)
{
	int i;
	int ret;
	char *cmdDup_p, *token_p;
	struct stat statInfo;

	if (verbose_G > 0)
		printf("number of CMD items: %d\n", cmdInfoCnt_G);

	if (cmdInfoCnt_G <= 0)
		return;

	for (i=0; i<cmdInfoCnt_G; ++i) {
		if (verbose_G > 0) {
			printf("CMD[%d]\n", i);
			printf("\tid: %s\n", cmdInfo_pG[i].cmdID_p);
			printf("\tcmd: %s\n", cmdInfo_pG[i].cmdStr_p);
		}

		cmdInfo_pG[i].valid = false;

		cmdDup_p = strdup(cmdInfo_pG[i].cmdStr_p);
		if (cmdDup_p == NULL) {
			printf("\t\tstdup() failure\n");
			continue;
		}
		token_p = strtok(cmdDup_p, " \t\n");
		if (token_p == NULL) {
			printf("\t\tstrtok() failure\n");
			continue;
		}

		ret = stat(cmdDup_p, &statInfo);
		if (ret != 0) {
			cmdInfo_pG[i].valid = false;
			printf("\t\tstat() failure, marked invalid\n");
			if (cmdDup_p != NULL)
				free(cmdDup_p);
			continue;
		}
		if (!S_ISREG(statInfo.st_mode)) {
			cmdInfo_pG[i].valid = false;
			printf("\t\tnot a regular file, marked invalid\n");
			if (cmdDup_p != NULL)
				free(cmdDup_p);
			continue;
		}
		if (!(statInfo.st_mode & S_IXOTH)) {
			cmdInfo_pG[i].valid = false;
			printf("\t\tnot executable, marked invalid\n");
			if (cmdDup_p != NULL)
				free(cmdDup_p);
			continue;
		}
		cmdInfo_pG[i].valid = true;
		printf("\tvalid: %s\n", cmdInfo_pG[i].valid? "yes" : "no");
		if (cmdDup_p != NULL)
			free(cmdDup_p);
	}
}

static void
init_SUBinfo (void)
{
	int i;

	if (verbose_G > 0)
		printf("number of SUB items: %d\n", subInfoCnt_G);

	if (subInfoCnt_G <= 0)
		return;

	for (i=0; i<subInfoCnt_G; ++i) {
		if (verbose_G > 0) {
			printf("SUB[%d]\n", i);
			printf("\ttopic: %s\n", subInfo_pG[i].topicStr_p);
			printf("\tlink: %s\n", subInfo_pG[i].linkID_p);
			printf("\tqos: %d\n", subInfo_pG[i].qos);
		}
	}
}

static void
init_mosquitto (void)
{
	int ret;
	int sleepSec = 1;

	ret = mosquitto_lib_init();
	if (ret != MOSQ_ERR_SUCCESS) {
		printf("can't initialize mosquitto library\n");
		exit(EXIT_FAILURE);
	}

	mosq_pG = mosquitto_new(NULL, true, NULL);
	if (mosq_pG == NULL) {
		perror("mosquitto_new()");
		exit(EXIT_FAILURE);
	}

	mosquitto_connect_callback_set(mosq_pG, connect_callback);
	mosquitto_message_callback_set(mosq_pG, process_message);

	// loop forever, if necessary, on first connection
	// on failure, wait 60 seconds before trying again
	while (1) {
		ret = mosquitto_connect(mosq_pG, mqttServer_pG, mqttServerPort_G, 10);
		if (ret == MOSQ_ERR_SUCCESS)
			break;
		sleep(sleepSec);
		if (sleepSec < 60)
			sleepSec *= 2;
	}
}

static void
cleanup (void)
{
	int i;

	if (mosq_pG != NULL) {
		mosquitto_destroy(mosq_pG);
		mosquitto_lib_cleanup();
	}

	if (userConfigFile_pG == defaultConfigFileName_pG)
		free(defaultConfigFileName_pG);

	if (mqttServer_pG != NULL)
		free (mqttServer_pG);

	if (gpioInfoCnt_G > 0) {
		for (i=gpioInfoCnt_G-1; i>=0; --i) {
			if (gpioInfo_pG[i].gpioID_p != NULL)
				free(gpioInfo_pG[i].gpioID_p);
			if (gpioInfo_pG[i].chipStr_p != NULL)
				free(gpioInfo_pG[i].chipStr_p);
			if (gpioInfo_pG[i].line_p != NULL)
				gpiod_line_release(gpioInfo_pG[i].line_p);
			if (gpioInfo_pG[i].chip_p != NULL)
				gpiod_chip_close(gpioInfo_pG[i].chip_p);
		}
		free(gpioInfo_pG);
	}

	if (subInfoCnt_G > 0) {
		for (i=subInfoCnt_G-1; i>=0; --i) {
			if (subInfo_pG[i].topicStr_p != NULL)
				free(subInfo_pG[i].topicStr_p);
			if (subInfo_pG[i].linkID_p != NULL)
				free(subInfo_pG[i].linkID_p);
		}
		free(subInfo_pG);
	}
}

static void
connect_callback (struct mosquitto *mosq_p, NOTU void *userdata_p, int result)
{
	int i, ret;

	if (!result) {
		if (verbose_G > 0)
			printf("connected!\n");

		for (i=0; i<subInfoCnt_G; ++i) {
			ret = mosquitto_subscribe(mosq_p, NULL, subInfo_pG[i].topicStr_p, subInfo_pG[i].qos);
			if (ret != MOSQ_ERR_SUCCESS)
				printf("can't subscribe to topic: '%s'\n", subInfo_pG[i].topicStr_p);
			else
				printf("subscribed to topic: '%s'\n", subInfo_pG[i].topicStr_p);
		}
	}
}

static void
process_message (NOTU struct mosquitto *mosq_p, NOTU void *userdata_p, const struct mosquitto_message *msg_p)
{
	int topic, gpio, cmd, val;

	// check payload
	val = -1;
	if (strncmp((char*)msg_p->payload, "ON", strlen((char*)msg_p->payload)) == 0)
		val = 1;
	if (strncmp((char*)msg_p->payload, "OFF", strlen((char*)msg_p->payload)) == 0)
		val = 0;
	if (val == -1) {
		printf("unhandled payload: '%s'\n", (char*)msg_p->payload);
		return;
	}

	for (topic=0; topic<subInfoCnt_G; ++topic) {
		if (strncmp(msg_p->topic, subInfo_pG[topic].topicStr_p, strlen(subInfo_pG[topic].topicStr_p)) == 0) {
			// check for any gpios with this topic
			for (gpio=0; gpio<gpioInfoCnt_G; ++gpio) {
				if (strncmp(subInfo_pG[topic].linkID_p, gpioInfo_pG[gpio].gpioID_p, strlen(gpioInfo_pG[gpio].gpioID_p)) == 0) {
					if (subInfo_pG[topic].inv) {
						if (val == 0)
							val = 1;
						else if (val == 1)
							val = 0;
					}
					if (verbose_G)
						printf("setting gpio chip %s pin %d to %d%s\n",
								gpioInfo_pG[gpio].chipStr_p,
								gpioInfo_pG[gpio].pin, val,
								subInfo_pG[topic].inv? " INV" : "");
					gpiod_line_set_value(gpioInfo_pG[gpio].line_p, val);
				}
			}

			// check for any *valid* cmds with this topic
			for (cmd=0; cmd<cmdInfoCnt_G; ++cmd) {
				if (!cmdInfo_pG[cmd].valid)
					continue;

				if (strncmp(subInfo_pG[topic].linkID_p, cmdInfo_pG[cmd].cmdID_p, strlen(cmdInfo_pG[cmd].cmdID_p)) == 0) {
					// process "ON" message
					if (val == 1) {
						pid_t pid;

						if (cmdInfo_pG[cmd].running) {
							if (verbose_G > 0)
								printf("not re-running an already-existing cmd: pid:%u\n", cmdInfo_pG[cmd].pid);
							break;
						}

						pid = fork();
						if (pid == 0) {
							// child
							execl(cmdInfo_pG[cmd].cmdStr_p, cmdInfo_pG[cmd].cmdStr_p, (char*)NULL);
						}
						else if (pid > 0) {
							// parent
							if (verbose_G > 0)
								printf("forking:'%s' as pid:%u\n", cmdInfo_pG[cmd].cmdStr_p, pid);
							cmdInfo_pG[cmd].pid = pid;
							cmdInfo_pG[cmd].running = true;

							if (cmdInfo_pG[cmd].oneshot) {
								if (verbose_G > 0)
									printf("oneshot detected, terminating pid %u\n", cmdInfo_pG[cmd].pid);
								waitpid(cmdInfo_pG[cmd].pid, NULL, 0);
								cmdInfo_pG[cmd].running = false;
							}
						}
						else {
							printf("fork() error\n");
							break;
						}
					}

					// process "OFF" message
					else {
						if (cmdInfo_pG[cmd].running) {
							if (verbose_G > 0)
								printf("terminating pid %u\n", cmdInfo_pG[cmd].pid);
							kill(cmdInfo_pG[cmd].pid, SIGTERM);
							waitpid(cmdInfo_pG[cmd].pid, NULL, 0);
							cmdInfo_pG[cmd].running = false;
						}
					}
				}
			}
		}
	}
}
