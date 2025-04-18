/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Hoernchen <la@tfc-server.de>
 * Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>
 * Copyright (C) 2012 by Youssef Touil <youssef@sdrsharp.com>
 * Copyright (C) 2012 by Ian Gilmour <ian@sdrsharp.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include "getopt/getopt.h"
#endif

#include <pthread.h>
#include <libusb.h>

/* MQTT Support */
#ifdef HAVE_MQTT
#include <mosquitto.h>
#endif

#include "rtl-sdr.h"
#include "convenience/convenience.h"

#ifdef _WIN32
#define sleep Sleep
#if defined(_MSC_VER) && (_MSC_VER < 1800)
#define round(x) (x > 0.0 ? floor(x + 0.5): ceil(x - 0.5))
#endif
#endif

#define ADSB_RATE			2000000
#define ADSB_FREQ			1090000000
#define DEFAULT_ASYNC_BUF_NUMBER	12
#define DEFAULT_BUF_LENGTH		(16 * 16384)
#define AUTO_GAIN			-100

#define MESSAGEGO    253
#define OVERWRITE    254
#define BADSAMPLE    255

#ifdef HAVE_MQTT
#define MQTT_DEFAULT_HOST       "localhost"
#define MQTT_DEFAULT_PORT       "1883"
#define MQTT_DEFAULT_TOPIC      "adsb/raw"
#define MQTT_DEFAULT_QOS        0
#define MQTT_DEFAULT_KEEPALIVE  60
#define MQTT_DEFAULT_TLS        0
#define MQTT_DEFAULT_CA_CERT    0
#endif

static pthread_t demod_thread;
static pthread_cond_t ready;
static pthread_mutex_t ready_m;
static volatile int do_exit = 0;
static rtlsdr_dev_t *dev = NULL;

uint16_t squares[256];

/* todo, bundle these up in a struct */
uint8_t *buffer;  /* also abused for uint16_t */
int verbose_output = 0;
int short_output = 0;
int quality = 10;
int allowed_errors = 5;
FILE *file;
int adsb_frame[14];
#define preamble_len		16
#define long_frame		112
#define short_frame		56

/* MQTT related variables */
#ifdef HAVE_MQTT
struct mqtt_config {
    int enabled;
    char *host;
    char *port;
    char *username;
    char *password;
    char *topic;
    int use_tls;
    int ca_cert;
    int qos;
    int keepalive;
};

static struct mqtt_config mqtt = {0};
static struct mosquitto *mosq = NULL;
#endif

/* signals are not threadsafe by default */
#define safe_cond_signal(n, m) pthread_mutex_lock(m); pthread_cond_signal(n); pthread_mutex_unlock(m)
#define safe_cond_wait(n, m) pthread_mutex_lock(m); pthread_cond_wait(n, m); pthread_mutex_unlock(m)

#ifdef HAVE_MQTT
void mqtt_cleanup(void) {
    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        mosq = NULL;
    }
    mosquitto_lib_cleanup();
}
#endif

void usage(void)
{
	fprintf(stderr,
		"rtl_adsb, a simple ADS-B decoder\n\n"
		"Use:\trtl_adsb [-R] [-g gain] [-p ppm] [output file]\n"
		"\t[-d device_index or serial (default: 0)]\n"
		"\t[-V verbove output (default: off)]\n"
		"\t[-S show short frames (default: off)]\n"
		"\t[-Q quality (0: no sanity checks, 0.5: half bit, 1: one bit (default), 2: two bits)]\n"
		"\t[-e allowed_errors (default: 5)]\n"
		"\t[-g tuner_gain (default: automatic)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\t[-T enable bias-T on GPIO PIN 0 (works for rtl-sdr.com v3 dongles)]\n"
#ifdef HAVE_MQTT
        "\t[-M enable MQTT output]\n"
        "\t[-H mqtt_host (default: localhost or MQTT_HOST env var)]\n"
        "\t[-P mqtt_port (default: 1883 or MQTT_PORT env var)]\n"
        "\t[-U mqtt_username (default: none or MQTT_USER env var)]\n"
        "\t[-W mqtt_password (default: none or MQTT_PASSWORD env var)]\n"
        "\t[-O mqtt_topic (default: adsb/raw or MQTT_TOPIC env var)]\n"
        "\t[-L enable TLS for MQTT connection (default: off or MQTT_TLS env var)]\n"
        "\t[-C check CA CERT for MQTT connection (default: off or MQTT_CA_CERT env var)]\n"
#endif
		"\tfilename (a '-' dumps samples to stdout)\n"
		"\t (omitting the filename also uses stdout)\n\n"
		"Streaming with netcat:\n"
		"\trtl_adsb | netcat -lp 8080\n"
		"\twhile true; do rtl_adsb | nc -lp 8080; done\n"
		"Streaming with socat:\n"
		"\trtl_adsb | socat -u - TCP4:sdrsharp.com:47806\n"
		"\n");
	exit(1);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	signal(SIGPIPE, SIG_IGN);
	fprintf(stderr, "Signal caught, exiting!\n");
	do_exit = 1;
	rtlsdr_cancel_async(dev);
}
#endif

#ifdef HAVE_MQTT
void mqtt_publish_message(const char *message) {
    if (!mqtt.enabled || !mosq)
        return;
    
    int result = mosquitto_publish(mosq, NULL, mqtt.topic, strlen(message), message, mqtt.qos, false);
    if (result != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "MQTT publish failed: %s\n", mosquitto_strerror(result));
    }
}
#endif

void display(int *frame, int len)
{
	int i, df;
	if (!short_output && len <= short_frame) {
		return;}
	df = (frame[0] >> 3) & 0x1f;
	if (quality == 0 && !(df==11 || df==17 || df==18 || df==19)) {
		return;}
    
    // Build the message output
    char adsb_msg[128] = "*";
    char *ptr = adsb_msg + 1;  // Start after the '*'
	for (i=0; i<((len+7)/8); i++) {
		ptr += sprintf(ptr, "%02x", frame[i]);
    }
    strcat(ptr, ";\r\n");
    
    // Output to file
	fprintf(file, "%s", adsb_msg);
    
    // Publish to MQTT if enabled
#ifdef HAVE_MQTT
    if (mqtt.enabled) {
        mqtt_publish_message(adsb_msg);
    }
#endif
    
	if (!verbose_output) {
		return;}
	fprintf(file, "DF=%i CA=%i\n", df, frame[0] & 0x07);
	fprintf(file, "ICAO Address=%06x\n", frame[1] << 16 | frame[2] << 8 | frame[3]);
	if (len <= short_frame) {
		return;}
	fprintf(file, "PI=0x%06x\n",  frame[11] << 16 | frame[12] << 8 | frame[13]);
	fprintf(file, "Type Code=%i S.Type/Ant.=%x\n", (frame[4] >> 3) & 0x1f, frame[4] & 0x07);
	fprintf(file, "--------------\n");
}

int abs8(int x)
/* do not subtract 127 from the raw iq, this handles it */
{
	if (x >= 127) {
		return x - 127;}
	return 127 - x;
}

void squares_precompute(void)
/* equiv to abs(x-128) ^ 2 */
{
	int i, j;
	// todo, check if this LUT is actually any faster
	for (i=0; i<256; i++) {
		j = abs8(i);
		squares[i] = (uint16_t)(j*j);
	}
}

int magnitute(uint8_t *buf, int len)
/* takes i/q, changes buf in place (16 bit), returns new len (16 bit) */
{
	int i;
	uint16_t *m;
	for (i=0; i<len; i+=2) {
		m = (uint16_t*)(&buf[i]);
		*m = squares[buf[i]] + squares[buf[i+1]];
	}
	return len/2;
}

static inline uint16_t single_manchester(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
/* takes 4 consecutive real samples, return 0 or 1, BADSAMPLE on error */
{
	int bit, bit_p;
	bit_p = a > b;
	bit   = c > d;

	if (quality == 0) {
		return bit;}

	if (quality == 5) {
		if ( bit &&  bit_p && b > c) {
			return BADSAMPLE;}
		if (!bit && !bit_p && b < c) {
			return BADSAMPLE;}
		return bit;
	}

	if (quality == 10) {
		if ( bit &&  bit_p && c > b) {
			return 1;}
		if ( bit && !bit_p && d < b) {
			return 1;}
		if (!bit &&  bit_p && d > b) {
			return 0;}
		if (!bit && !bit_p && c < b) {
			return 0;}
		return BADSAMPLE;
	}

	if ( bit &&  bit_p && c > b && d < a) {
		return 1;}
	if ( bit && !bit_p && c > a && d < b) {
		return 1;}
	if (!bit &&  bit_p && c < a && d > b) {
		return 0;}
	if (!bit && !bit_p && c < b && d > a) {
		return 0;}
	return BADSAMPLE;
}

static inline uint16_t min16(uint16_t a, uint16_t b)
{
	return a<b ? a : b;
}

static inline uint16_t max16(uint16_t a, uint16_t b)
{
	return a>b ? a : b;
}

static inline int preamble(uint16_t *buf, int i)
/* returns 0/1 for preamble at index i */
{
	int i2;
	uint16_t low  = 0;
	uint16_t high = 65535;
	for (i2=0; i2<preamble_len; i2++) {
		switch (i2) {
			case 0:
			case 2:
			case 7:
			case 9:
				//high = min16(high, buf[i+i2]);
				high = buf[i+i2];
				break;
			default:
				//low  = max16(low,  buf[i+i2]);
				low = buf[i+i2];
				break;
		}
		if (high <= low) {
			return 0;}
	}
	return 1;
}

void manchester(uint16_t *buf, int len)
/* overwrites magnitude buffer with valid bits (BADSAMPLE on errors) */
{
	/* a and b hold old values to verify local manchester */
	uint16_t a=0, b=0;
	uint16_t bit;
	int i, i2, start, errors;
	int maximum_i = len - 1;        // len-1 since we look at i and i+1
	// todo, allow wrap across buffers
	i = 0;
	while (i < maximum_i) {
		/* find preamble */
		for ( ; i < (len - preamble_len); i++) {
			if (!preamble(buf, i)) {
				continue;}
			a = buf[i];
			b = buf[i+1];
			for (i2=0; i2<preamble_len; i2++) {
				buf[i+i2] = MESSAGEGO;}
			i += preamble_len;
			break;
		}
		i2 = start = i;
		errors = 0;
		/* mark bits until encoding breaks */
		for ( ; i < maximum_i; i+=2, i2++) {
			bit = single_manchester(a, b, buf[i], buf[i+1]);
			a = buf[i];
			b = buf[i+1];
			if (bit == BADSAMPLE) {
				errors += 1;
				if (errors > allowed_errors) {
					buf[i2] = BADSAMPLE;
					break;
				} else {
					bit = a > b;
					/* these don't have to match the bit */
					a = 0;
					b = 65535;
				}
			}
			buf[i] = buf[i+1] = OVERWRITE;
			buf[i2] = bit;
		}
	}
}

void messages(uint16_t *buf, int len)
{
	int i, data_i, index, shift, frame_len;
	// todo, allow wrap across buffers
	for (i=0; i<len; i++) {
		if (buf[i] > 1) {
			continue;}
		frame_len = long_frame;
		data_i = 0;
		for (index=0; index<14; index++) {
			adsb_frame[index] = 0;}
		for(; i<len && buf[i]<=1 && data_i<frame_len; i++, data_i++) {
			if (buf[i]) {
				index = data_i / 8;
				shift = 7 - (data_i % 8);
				adsb_frame[index] |= (uint8_t)(1<<shift);
			}
			if (data_i == 7) {
				if (adsb_frame[0] == 0) {
				    break;}
				if (adsb_frame[0] & 0x80) {
					frame_len = long_frame;}
				else {
					frame_len = short_frame;}
			}
		}
		if (data_i < (frame_len-1)) {
			continue;}
		display(adsb_frame, frame_len);
		fflush(file);
	}
}

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	if (do_exit) {
		return;}
	memcpy(buffer, buf, len);
	safe_cond_signal(&ready, &ready_m);
}

static void *demod_thread_fn(void *arg)
{
	int len;
	while (!do_exit) {
		safe_cond_wait(&ready, &ready_m);
		len = magnitute(buffer, DEFAULT_BUF_LENGTH);
		manchester((uint16_t*)buffer, len);
		messages((uint16_t*)buffer, len);
	}
	rtlsdr_cancel_async(dev);
	return 0;
}

#ifdef HAVE_MQTT
int init_mqtt(void) {
    if (!mqtt.enabled)
        return 0;

    mosquitto_lib_init();

    // Create a new client instance
    mosq = mosquitto_new(NULL, true, NULL);
    if (!mosq) {
        fprintf(stderr, "Error: Out of memory when creating MQTT client.\n");
        return -1;
    }

    // Set username and password if provided
    if (mqtt.username && mqtt.password) {
        if (mosquitto_username_pw_set(mosq, mqtt.username, mqtt.password) != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "Error setting MQTT username and password\n");
            mqtt_cleanup();
            return -1;
        }
    }

    // Set TLS if requested
    if (mqtt.use_tls) {
        if (mosquitto_tls_set(mosq, NULL, NULL, NULL, NULL, NULL) != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "Error setting MQTT TLS\n");
            mqtt_cleanup();
            return -1;
        }
    }

    // Connect to the broker
    int result = mosquitto_connect(mosq, mqtt.host, atoi(mqtt.port), mqtt.keepalive);
    if (result != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Error connecting to MQTT broker %s:%s: %s\n", 
                mqtt.host, mqtt.port, mosquitto_strerror(result));
        mqtt_cleanup();
        return -1;
    }

    // Start the MQTT client loop in a background thread
    result = mosquitto_loop_start(mosq);
    if (result != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "Error starting MQTT loop: %s\n", mosquitto_strerror(result));
        mqtt_cleanup();
        return -1;
    }

    fprintf(stderr, "MQTT client connected to %s:%s, topic: %s\n", 
            mqtt.host, mqtt.port, mqtt.topic);
    return 0;
}

char *get_env_or_default(const char *env_var, const char *default_val) {
    char *value = getenv(env_var);
    if (value)
        return strdup(value);
    if (default_val)
        return strdup(default_val);
    return NULL;
}

void init_mqtt_config(void) {
    // Set defaults from environment variables if available
    if (!mqtt.host)
        mqtt.host = get_env_or_default("MQTT_HOST", "localhost");
    
    if (!mqtt.port)
        mqtt.port = get_env_or_default("MQTT_PORT", MQTT_DEFAULT_PORT);
    
    if (!mqtt.username)
        mqtt.username = get_env_or_default("MQTT_USER", NULL);
    
    if (!mqtt.password)
        mqtt.password = get_env_or_default("MQTT_PASSWORD", NULL);
    
    if (!mqtt.topic)
        mqtt.topic = get_env_or_default("MQTT_TOPIC", MQTT_DEFAULT_TOPIC);
    
    if (!mqtt.use_tls) {
        char *tls_env = getenv("MQTT_TLS");
        if (tls_env && (strcmp(tls_env, "1") == 0 || strcasecmp(tls_env, "true") == 0 || 
                        strcasecmp(tls_env, "yes") == 0))
            mqtt.use_tls = 1;
    }

    if (!mqtt.ca_cert) {
        char *ca_cert = getenv("MQTT_CA_CERT");
        if (ca_cert && (strcmp(ca_cert, "1") == 0 || strcasecmp(ca_cert, "true") == 0 || 
                        strcasecmp(ca_cert, "yes") == 0))
            mqtt.ca_cert= 1;
    }
    
    // Set other defaults
    if (mqtt.qos == 0)
        mqtt.qos = MQTT_DEFAULT_QOS;
    
    if (mqtt.keepalive == 0)
        mqtt.keepalive = MQTT_DEFAULT_KEEPALIVE;
}

void cleanup_mqtt_config(void) {
    if (mqtt.host) free(mqtt.host);
    if (mqtt.port) free(mqtt.port);
    if (mqtt.username) free(mqtt.username);
    if (mqtt.password) free(mqtt.password);
    if (mqtt.topic) free(mqtt.topic);
}
#endif

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact;
#endif
	char *filename = NULL;
	int r, opt;
	int gain = AUTO_GAIN; /* tenths of a dB */
	int dev_index = 0;
	int dev_given = 0;
	int ppm_error = 0;
	int enable_biastee = 0;
	pthread_cond_init(&ready, NULL);
	pthread_mutex_init(&ready_m, NULL);
	squares_precompute();

#ifdef HAVE_MQTT
	while ((opt = getopt(argc, argv, "d:g:p:e:Q:VSTMH:P:U:W:O:L")) != -1)
#else
	while ((opt = getopt(argc, argv, "d:g:p:e:Q:VST")) != -1)
#endif
	{
		switch (opt) {
		case 'd':
			dev_index = verbose_device_search(optarg);
			dev_given = 1;
			break;
		case 'g':
			gain = (int)(atof(optarg) * 10);
			break;
		case 'p':
			ppm_error = atoi(optarg);
			break;
		case 'V':
			verbose_output = 1;
			break;
		case 'S':
			short_output = 1;
			break;
		case 'e':
			allowed_errors = atoi(optarg);
			break;
		case 'Q':
			quality = (int)(atof(optarg) * 10);
			break;
		case 'T':
			enable_biastee = 1;
			break;
#ifdef HAVE_MQTT
        case 'M':
            mqtt.enabled = 1;
            break;
        case 'H':
            if (mqtt.host) free(mqtt.host);
            mqtt.host = strdup(optarg);
            break;
        case 'P':
            if (mqtt.port) free(mqtt.port);
            mqtt.port = strdup(optarg);
            break;
        case 'U':
            if (mqtt.username) free(mqtt.username);
            mqtt.username = strdup(optarg);
            break;
        case 'W':
            if (mqtt.password) free(mqtt.password);
            mqtt.password = strdup(optarg);
            break;
        case 'O':
            if (mqtt.topic) free(mqtt.topic);
            mqtt.topic = strdup(optarg);
            break;
        case 'L':
            mqtt.use_tls = 1;
            break;
        case 'C':
            mqtt.ca_cert = 1;
            break;
#endif
		default:
			usage();
			return 0;
		}
	}

	if (argc <= optind) {
		filename = "-";
	} else {
		filename = argv[optind];
	}

	buffer = malloc(DEFAULT_BUF_LENGTH * sizeof(uint8_t));

	if (!dev_given) {
		dev_index = verbose_device_search("0");
	}

	if (dev_index < 0) {
		exit(1);
	}

	r = rtlsdr_open(&dev, (uint32_t)dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
		exit(1);
	}
#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

#ifdef HAVE_MQTT
    // Initialize MQTT if enabled
    if (mqtt.enabled) {
        init_mqtt_config();
        if (init_mqtt() != 0) {
            fprintf(stderr, "Failed to initialize MQTT client, continuing without MQTT support\n");
            mqtt.enabled = 0;
        }
    }
#endif

	if (strcmp(filename, "-") == 0) { /* Write samples to stdout */
		file = stdout;
		setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
		_setmode(_fileno(file), _O_BINARY);
#endif
	} else {
		file = fopen(filename, "wb");
		if (!file) {
			fprintf(stderr, "Failed to open %s\n", filename);
			exit(1);
		}
	}

	/* Set the tuner gain */
	if (gain == AUTO_GAIN) {
		verbose_auto_gain(dev);
	} else {
		gain = nearest_gain(dev, gain);
		verbose_gain_set(dev, gain);
	}

	verbose_ppm_set(dev, ppm_error);
	r = rtlsdr_set_agc_mode(dev, 1);

	/* Set the tuner frequency */
	verbose_set_frequency(dev, ADSB_FREQ);

	/* Set the sample rate */
	verbose_set_sample_rate(dev, ADSB_RATE);

	rtlsdr_set_bias_tee(dev, enable_biastee);
	if (enable_biastee)
		fprintf(stderr, "activated bias-T on GPIO PIN 0\n");

	/* Reset endpoint before we start reading from it (mandatory) */
	verbose_reset_buffer(dev);

	pthread_create(&demod_thread, NULL, demod_thread_fn, (void *)(NULL));
	rtlsdr_read_async(dev, rtlsdr_callback, (void *)(NULL),
			      DEFAULT_ASYNC_BUF_NUMBER,
			      DEFAULT_BUF_LENGTH);

	if (do_exit) {
		fprintf(stderr, "\nUser cancel, exiting...\n");}
	else {
		fprintf(stderr, "\nLibrary error %d, exiting...\n", r);}
	rtlsdr_cancel_async(dev);
	pthread_cancel(demod_thread);
	pthread_join(demod_thread, NULL);
	pthread_cond_destroy(&ready);
	pthread_mutex_destroy(&ready_m);

	if (file != stdout) {
		fclose(file);}

#ifdef HAVE_MQTT
    // Clean up MQTT resources
    if (mqtt.enabled) {
        mqtt_cleanup();
        cleanup_mqtt_config();
    }
#endif

	rtlsdr_close(dev);
	free(buffer);
	return r >= 0 ? r : -r;
}
