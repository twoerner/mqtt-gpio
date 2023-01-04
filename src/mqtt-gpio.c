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
#include <gpiod.h>
#include <mosquitto.h>

#include "config.h"

#define NOTU __attribute__((unused))
#define DEFAULT_CONFIG_FILE "/mqtt-gpio.conf"

typedef struct {
	char *gpioName;
	char *chipStr;
	struct gpiod_chip *chip;
	int pin;
	struct gpiod_line *line;
	struct gpiod_line_request_config config;
} GPIOinfo_t;

typedef struct {
	char *topicStr;
	char *gpioName;
	int qos;
} SUBinfo_t;

static char *defaultConfigFileName_G = NULL;
static char *userConfigFile_G = NULL;
static int verbose_G = 0;
static GPIOinfo_t *gpioInfo_G = NULL;
static int gpioInfoCnt_G = 0;
static SUBinfo_t *subInfo_G = NULL;
static int subInfoCnt_G = 0;
static char *mqttServer_G = NULL;
static int mqttServerPort_G = 0;
static struct mosquitto *mosq_G = NULL;

static void usage (char *pgm);
static void parse_cmdline (int argc, char *argv[]);
static void set_default_config_filename (void);
static void process_config_file (void);
static void init_SUBinfo (void);
static void init_GPIOinfo (void);
static void init_mosquitto (void);
static void cleanup (void);
static void connect_callback (struct mosquitto *mosq, void *userdata, int result);
static void process_message (struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg);

int
main (int argc, char *argv[])
{
	atexit(cleanup);

	set_default_config_filename();
	parse_cmdline(argc,argv);
	process_config_file();
	init_GPIOinfo();
	init_SUBinfo();
	init_mosquitto();
	mosquitto_loop_forever(mosq_G, -1, 1);

	return EXIT_SUCCESS;
}

static void
usage (char *pgm)
{
	printf("usage: %s [OPTIONS]\n", pgm);
	printf("  where <OPTIONS> are:\n");
	printf("    -h | --help        Print help options to terminal and exit successfully\n");
	printf("    -v | --version     Show program version information and exit successfully\n");
	printf("    -V | --verbose     Run program verbosely, use multiple for more verbosity\n");
	printf("    -c | --config <f>  Use <f> for config instead of default (%s)\n",
			defaultConfigFileName_G);
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
				free(defaultConfigFileName_G);
				defaultConfigFileName_G = NULL;
				userConfigFile_G = optarg;
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
	defaultConfigFileName_G = (char*)malloc(len + 1);
	if (defaultConfigFileName_G == NULL) {
		perror("malloc()");
		exit(EXIT_FAILURE);
	}
	memset(defaultConfigFileName_G, 0, len+1);
	defaultConfigFileName_G = strcat(defaultConfigFileName_G, ETCPKGDIR);
	defaultConfigFileName_G = strcat(defaultConfigFileName_G, DEFAULT_CONFIG_FILE);
	userConfigFile_G = defaultConfigFileName_G;
}

static void
process_config_file (void)
{
	FILE *stream;
	char *line = NULL;
	size_t len = 0;
	ssize_t nread;
	const char *delim = " \t\n";
	char *token;
	unsigned lineCnt;

	if (userConfigFile_G == NULL) {
		printf("no config file specified\n");
		exit(EXIT_FAILURE);
	}

	stream = fopen(userConfigFile_G, "r");
	if (stream == NULL) {
		perror("fopen()");
		printf("%s\n", userConfigFile_G);
		exit(EXIT_FAILURE);
	}

	lineCnt = 0;
	while ((nread = getline(&line, &len, stream)) != -1) {
		++lineCnt;

		if (verbose_G > 1)
			printf("config[%03d]: %s", lineCnt, line);

		// skip blank lines and lines starting with '#'
		if (line[0] == '#') {
			if (verbose_G > 1)
				printf(" skipping comment\n");
			continue;
		}
		if (line[0] == '\n') {
			if (verbose_G > 1)
				printf(" skipping empty line\n");
			continue;
		}

		token = strtok(line, delim);
		if (token == NULL) {
			printf("   invalid config line #%d: no CMD\n", lineCnt);
			continue;
		}

		// MQTT
		if (strcmp(token, "MQTT") == 0) {
			if (verbose_G)
				printf("found an MQTT\n");

			// server DNS/IP
			token = strtok(NULL, delim);
			if (token == NULL) {
				printf("   invalid config line #%d: MQTT server DNS/IP expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G)
				printf("   MQTT server DNS/IP: %s\n", token);
			mqttServer_G = strdup(token);
			if (mqttServer_G == NULL) {
				perror("strdup(MQTT server)");
				exit(EXIT_FAILURE);
			}

			// server port
			token = strtok(NULL, delim);
			if (token == NULL) {
				printf("   invalid config line #%d: MQTT server port expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			mqttServerPort_G = atoi(token);
			if (verbose_G)
				printf("   MQTT port: %d\n", mqttServerPort_G);

			continue;
		}

		// GPIO
		if (strcmp(token, "GPIO") == 0) {
			if (verbose_G > 1)
				printf(" found a GPIO (cnt:%u)\n", gpioInfoCnt_G);

			if ((gpioInfoCnt_G+1) == INT_MAX) {
				printf("   no more room in GPIO table, not added\n");
				continue;
			}
			gpioInfo_G = (GPIOinfo_t*)realloc(gpioInfo_G,
					((gpioInfoCnt_G+1) * sizeof(GPIOinfo_t)));
			if (gpioInfo_G == NULL) {
				perror("realloc(GPIO)");
				exit(EXIT_FAILURE);
			}
			memset(&gpioInfo_G[gpioInfoCnt_G], 0, sizeof(GPIOinfo_t));
			if (verbose_G > 1)
				printf("   realloc(GPIO)'ed\n");

			// gpio name
			token = strtok(NULL, delim);
			if (token == NULL) {
				printf("   invalid config line #%d: gpio name expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   gpio name: %s\n", token);
			gpioInfo_G[gpioInfoCnt_G].gpioName = strdup(token);
			if (gpioInfo_G[gpioInfoCnt_G].gpioName == NULL) {
				perror("strdup(gpio name)");
				exit(EXIT_FAILURE);
			}

			// chip
			token = strtok(NULL, delim);
			if (token == NULL) {
				printf("   invalid config line #%d: chip expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   chip: %s\n", token);
			gpioInfo_G[gpioInfoCnt_G].chipStr = strdup(token);
			if (gpioInfo_G[gpioInfoCnt_G].chipStr == NULL) {
				perror("strdup(chip)");
				exit(EXIT_FAILURE);
			}

			// pin
			token = strtok(NULL, delim);
			if (token == NULL) {
				printf("   invalid config line #%d: pin expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   pin: %s\n", token);
			gpioInfo_G[gpioInfoCnt_G].pin = atoi(token);

			++gpioInfoCnt_G;
			continue;
		}

		// SUB
		if (strcmp(token, "SUB") == 0) {
			if (verbose_G > 1)
				printf(" found a SUB (cnt:%u)\n", subInfoCnt_G);

			if ((subInfoCnt_G+1) == INT_MAX) {
				printf("   no more room in SUB table, not added\n");
				continue;
			}
			subInfo_G = (SUBinfo_t*)realloc(subInfo_G,
					((subInfoCnt_G+1) * sizeof(SUBinfo_t)));
			if (subInfo_G == NULL) {
				perror("realloc(SUB)");
				exit(EXIT_FAILURE);
			}
			memset(&subInfo_G[subInfoCnt_G], 0, sizeof(SUBinfo_t));
			if (verbose_G > 1)
				printf("   realloc(SUB)'ed\n");

			// topic
			token = strtok(NULL, delim);
			if (token == NULL) {
				printf("   invalid config line #%d: topic expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   topic: %s\n", token);
			subInfo_G[subInfoCnt_G].topicStr = strdup(token);
			if (subInfo_G[subInfoCnt_G].topicStr == NULL) {
				perror("strdup(topic)");
				exit(EXIT_FAILURE);
			}

			// gpio name
			token = strtok(NULL, delim);
			if (token == NULL) {
				printf("   invalid config line #%d: gpio name expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   gpio name: %s\n", token);
			subInfo_G[subInfoCnt_G].gpioName = strdup(token);
			if (subInfo_G[subInfoCnt_G].gpioName == NULL) {
				perror("strdup(gpio name)");
				exit(EXIT_FAILURE);
			}

			// qos
			token = strtok(NULL, delim);
			if (token == NULL) {
				printf("   invalid config line #%d: qos expected\n", lineCnt);
				exit(EXIT_FAILURE);
			}
			if (verbose_G > 1)
				printf("   qos: %s\n", token);
			subInfo_G[subInfoCnt_G].qos = atoi(token);

			++subInfoCnt_G;
			continue;
		}

		printf("   invalid config line #%d: unknown CMD: %s\n", lineCnt, token);
		exit(EXIT_FAILURE);
	}

	free(line);
	fclose(stream);
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
			printf("\tchip: %s\n", gpioInfo_G[i].chipStr);
			printf("\tpin: %d\n", gpioInfo_G[i].pin);
		}

		// open chip
		gpioInfo_G[i].chip = gpiod_chip_open_lookup(gpioInfo_G[i].chipStr);
		if (gpioInfo_G[i].chip == NULL) {
			printf("can't open gpio device: %s\n", gpioInfo_G[i].chipStr);
			exit(EXIT_FAILURE);
		}

		// get line
		gpioInfo_G[i].line = gpiod_chip_get_line(gpioInfo_G[i].chip, gpioInfo_G[i].pin);
		if (gpioInfo_G[i].line == NULL) {
			printf("can't get pin: %d\n", gpioInfo_G[i].pin);
			exit(EXIT_FAILURE);
		}

		// set config (direction)
		gpioInfo_G[i].config.request_type = GPIOD_LINE_REQUEST_DIRECTION_OUTPUT;
		ret = gpiod_line_request(gpioInfo_G[i].line, &gpioInfo_G[i].config, 0);
		if (ret != 0) {
			printf("can't set configuration for subscription %d\n", i);
			exit(EXIT_FAILURE);
		}
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
			printf("\ttopic: %s\n", subInfo_G[i].topicStr);
			printf("\tgpio: %s\n", subInfo_G[i].gpioName);
			printf("\tqos: %d\n", subInfo_G[i].qos);
		}
	}
}

static void
init_mosquitto (void)
{
	int ret;

	ret = mosquitto_lib_init();
	if (ret != MOSQ_ERR_SUCCESS) {
		printf("can't initialize mosquitto library\n");
		exit(EXIT_FAILURE);
	}

	mosq_G = mosquitto_new(NULL, true, NULL);
	if (mosq_G == NULL) {
		perror("mosquitto_new()");
		exit(EXIT_FAILURE);
	}

	mosquitto_connect_callback_set(mosq_G, connect_callback);
	mosquitto_message_callback_set(mosq_G, process_message);

	ret = mosquitto_connect(mosq_G, mqttServer_G, mqttServerPort_G, 10);
	if (ret != MOSQ_ERR_SUCCESS) {
		printf("can't connect to broker:%s port:%d\n", mqttServer_G, mqttServerPort_G);
		exit(EXIT_FAILURE);
	}
}

static void
cleanup (void)
{
	int i;

	if (mosq_G != NULL) {
		mosquitto_destroy(mosq_G);
		mosquitto_lib_cleanup();
	}

	if (userConfigFile_G == defaultConfigFileName_G)
		free(defaultConfigFileName_G);

	if (mqttServer_G != NULL)
		free (mqttServer_G);

	if (gpioInfoCnt_G > 0) {
		for (i=gpioInfoCnt_G-1; i>=0; --i) {
			if (gpioInfo_G[i].gpioName != NULL)
				free(gpioInfo_G[i].gpioName);
			if (gpioInfo_G[i].chipStr != NULL)
				free(gpioInfo_G[i].chipStr);
			if (gpioInfo_G[i].line != NULL)
				gpiod_line_release(gpioInfo_G[i].line);
			if (gpioInfo_G[i].chip != NULL)
				gpiod_chip_close(gpioInfo_G[i].chip);
		}
		free(gpioInfo_G);
	}

	if (subInfoCnt_G > 0) {
		for (i=subInfoCnt_G-1; i>=0; --i) {
			if (subInfo_G[i].topicStr != NULL)
				free(subInfo_G[i].topicStr);
			if (subInfo_G[i].gpioName != NULL)
				free(subInfo_G[i].gpioName);
		}
		free(subInfo_G);
	}
}

static void
connect_callback (struct mosquitto *mosq, NOTU void *userdata, int result)
{
	int i, ret;

	if (!result) {
		if (verbose_G > 0)
			printf("connected!\n");

		for (i=0; i<subInfoCnt_G; ++i) {
			ret = mosquitto_subscribe(mosq, NULL, subInfo_G[i].topicStr, subInfo_G[i].qos);
			if (ret != MOSQ_ERR_SUCCESS)
				printf("can't subscribe to topic: '%s'\n", subInfo_G[i].topicStr);
			else
				printf("subscribed to topic: '%s'\n", subInfo_G[i].topicStr);
		}
	}
}

static void
process_message (NOTU struct mosquitto *mosq, NOTU void *userdata, const struct mosquitto_message *msg)
{
	int topic, gpio, val;

	// check payload
	val = -1;
	if (strncmp((char*)msg->payload, "ON", strlen((char*)msg->payload)) == 0)
		val = 1;
	if (strncmp((char*)msg->payload, "OFF", strlen((char*)msg->payload)) == 0)
		val = 0;
	if (val == -1) {
		printf("unhandled payload: '%s'\n", (char*)msg->payload);
		return;
	}

	for (topic=0; topic<subInfoCnt_G; ++topic) {
		if (strncmp(msg->topic, subInfo_G[topic].topicStr, strlen(subInfo_G[topic].topicStr)) == 0) {
			for (gpio=0; gpio<gpioInfoCnt_G; ++gpio) {
				if (strncmp(subInfo_G[topic].gpioName, gpioInfo_G[gpio].gpioName, strlen(gpioInfo_G[gpio].gpioName)) == 0) {
					if (verbose_G)
						printf("setting gpio chip %s pin %d to %d\n",
								gpioInfo_G[gpio].chipStr,
								gpioInfo_G[gpio].pin, val);
					gpiod_line_set_value(gpioInfo_G[gpio].line, val);
				}
			}
		}
	}
}
