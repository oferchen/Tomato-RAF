/*
  Mode switching tool for controlling flip flop (multiple device) USB gear
  Version 1.2.7, 2013/08/07

  Copyright (C) 2007 - 2013 Josua Dietze (mail to "usb_admin" at the domain
  of the home page; or write a personal message through the forum to "Josh".
  NO SUPPORT VIA E-MAIL - please use the forum for that)

  Major contributions:

  Command line parsing, decent usage/config output/handling, bugfixes and advanced
  options added by:
    Joakim Wennergren

  TargetClass parameter implementation to support new Option devices/firmware:
    Paul Hardwick (http://www.pharscape.org)

  Created with initial help from:
    "usbsnoop2libusb.pl" by Timo Lindfors (http://iki.fi/lindi/usb/usbsnoop2libusb.pl)

  Config file parsing stuff borrowed from:
    Guillaume Dargaud (http://www.gdargaud.net/Hack/SourceCode.html)

  Hexstr2bin function borrowed from:
    Jouni Malinen (http://hostap.epitest.fi/wpa_supplicant, from "common.c")

  Other contributions: see README

  Device information contributors are named in the "device_reference.txt" file. See
  homepage.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details:

  http://www.gnu.org/licenses/gpl.txt

*/

/* Recommended tab size: 4 */

#define VERSION "1.2.7"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <getopt.h>
#include <syslog.h>
#include <unistd.h>

#include "usb_modeswitch.h"

#define LIBUSB_HAS_DETACH_KERNEL_DRIVER_NP
#define LIBUSB_HAS_GET_DRIVER_NP

#define USB_ENDPOINT_IN		LIBUSB_ENDPOINT_IN
#define USB_ENDPOINT_OUT		LIBUSB_ENDPOINT_OUT
#define USB_ENDPOINT_TYPE_MASK	LIBUSB_ENDPOINT_ADDRESS_MASK
#define USB_ENDPOINT_DIR_MASK	LIBUSB_ENDPOINT_DIR_MASK
#define USB_ENDPOINT_TYPE_BULK	LIBUSB_TRANSFER_TYPE_BULK
#define USB_TYPE_STANDARD		LIBUSB_REQUEST_TYPE_STANDARD
#define USB_TYPE_CLASS		LIBUSB_REQUEST_TYPE_CLASS
#define USB_TYPE_VENDOR		LIBUSB_REQUEST_TYPE_VENDOR
#define USB_RECIP_DEVICE		LIBUSB_RECIPIENT_DEVICE
#define USB_RECIP_INTERFACE		LIBUSB_RECIPIENT_INTERFACE
#define USB_REQ_SET_FEATURE		LIBUSB_REQUEST_SET_FEATURE
#define USB_REQ_GET_CONFIGURATION	LIBUSB_REQUEST_GET_CONFIGURATION

#define usb_set_debug(x)	libusb_set_debug(ctx, 3)
#define usb_close		libusb_close
#define usb_get_string_simple	libusb_get_string_descriptor_ascii
#define usb_reset		libusb_reset_device
#define usb_claim_interface	libusb_claim_interface
#define usb_clear_halt	libusb_clear_halt
#define usb_release_interface	libusb_release_interface
#define usb_control_msg	libusb_control_transfer
#define usb_set_configuration	libusb_set_configuration
#define usb_detach_kernel_driver_np	libusb_detach_kernel_driver

/* libusb 1.0 wrappers */
int usb_bulk_io(struct libusb_device_handle *handle, int ep, char *bytes,
	int size, int timeout)
{
	int actual_length;
	int r;
//	usbi_dbg("endpoint %x size %d timeout %d", ep, size, timeout);
	r = libusb_bulk_transfer(handle, ep & 0xff, bytes, size,
		&actual_length, timeout);
	
	/* if we timed out but did transfer some data, report as successful short
	 * read. FIXME: is this how libusb-0.1 works? */
	if (r == 0 || (r == LIBUSB_ERROR_TIMEOUT && actual_length > 0))
		return actual_length;

	return r;
}

int usb_bulk_read(struct libusb_device_handle *handle, int ep, char *bytes,
	int size, int timeout)
{
	if (!(ep & LIBUSB_ENDPOINT_IN)) {
		/* libusb-0.1 will strangely fix up a read request from endpoint
		 * 0x01 to be from endpoint 0x81. do the same thing here, but
		 * warn about this silly behaviour. */
		printf("endpoint %x is missing IN direction bit, fixing", ep);
		ep |= LIBUSB_ENDPOINT_IN;
	}

	return usb_bulk_io(handle, ep, bytes, size, timeout);
}

int usb_bulk_write(struct libusb_device_handle *handle, int ep, char *bytes,
	int size, int timeout)
{
	if (ep & LIBUSB_ENDPOINT_IN) {
		/* libusb-0.1 on BSD strangely fix up a write request to endpoint
		 * 0x81 to be to endpoint 0x01. do the same thing here, but
		 * warn about this silly behaviour. */
		printf("endpoint %x has excessive IN direction bit, fixing", ep);
		ep &= ~LIBUSB_ENDPOINT_IN;
	}

	return usb_bulk_io(handle, ep, bytes, size, timeout);
}

static int usb_interrupt_io(libusb_device_handle *handle, int ep, char *bytes,
	int size, int timeout)
{
	int actual_length;
	int r;
//	usbi_dbg("endpoint %x size %d timeout %d", ep, size, timeout);
	r = libusb_interrupt_transfer(handle, ep & 0xff, bytes, size,
		&actual_length, timeout);
	
	/* if we timed out but did transfer some data, report as successful short
	 * read. FIXME: is this how libusb-0.1 works? */
	if (r == 0 || (r == LIBUSB_ERROR_TIMEOUT && actual_length > 0))
		return actual_length;

	return (r);
}

int usb_interrupt_read(libusb_device_handle *handle, int ep, char *bytes,
	int size, int timeout)
{
	if (!(ep & USB_ENDPOINT_IN)) {
		/* libusb-0.1 will strangely fix up a read request from endpoint
		 * 0x01 to be from endpoint 0x81. do the same thing here, but
		 * warn about this silly behaviour. */
		printf("endpoint %x is missing IN direction bit, fixing", ep);
		ep |= USB_ENDPOINT_IN;
	}
	return usb_interrupt_io(handle, ep, bytes, size, timeout);
}

int usb_interrupt_write(libusb_device_handle *handle, int ep, char *bytes,
	int size, int timeout)
{
	if (ep & USB_ENDPOINT_IN) {
		/* libusb-0.1 on BSD strangely fix up a write request to endpoint
		 * 0x81 to be to endpoint 0x01. do the same thing here, but
		 * warn about this silly behaviour. */
		printf("endpoint %x has excessive IN direction bit, fixing", ep);
		ep &= ~USB_ENDPOINT_IN;
	}

	return usb_interrupt_io(handle, ep, bytes, size, timeout);
}

int usb_get_driver_np(struct libusb_device_handle *dev, int interface,
char *name, unsigned int namelen)
{
	int r = libusb_kernel_driver_active(dev, interface);
	if (r == 1) {
		/* libusb-1.0 doesn't expose driver name, so fill in a dummy value */
		snprintf(name, namelen, "dummy");
		return 0;
	} else return r;
}

#define LINE_DIM 1024
#define MAXLINES 50
#define BUF_SIZE 4096
#define DESCR_MAX 129

#define SEARCH_DEFAULT 0
#define SEARCH_TARGET 1
#define SEARCH_BUSDEV 2

#define SWITCH_CONFIG_MAXTRIES   5

#define SHOW_PROGRESS if (show_progress) fprintf

char *TempPP=NULL;

static struct libusb_context *ctx = NULL;
static struct libusb_device *dev;
static struct libusb_device_handle *devh;

int DefaultVendor=0, DefaultProduct=0, TargetVendor=0, TargetProduct=-1, TargetClass=0;
int MessageEndpoint=0, ResponseEndpoint=0, ReleaseDelay=0;
int targetDeviceCount=0, searchMode;
int devnum=-1, busnum=-1;
int ret;

char DetachStorageOnly=0, HuaweiMode=0, SierraMode=0, SonyMode=0, GCTMode=0, KobilMode=0;
char SequansMode=0, MobileActionMode=0, CiscoMode=0, QisdaMode=0, QuantaMode=0;
char BlackberryMode=0, PantechMode=0;
char verbose=0, show_progress=1, ResetUSB=0, CheckSuccess=0, config_read=0;
char NeedResponse=0, NoDriverLoading=0, InquireDevice=1, sysmode=0, mbim=0;

char imanufact[DESCR_MAX], iproduct[DESCR_MAX], iserial[DESCR_MAX];

char MessageContent[LINE_DIM];
char MessageContent2[LINE_DIM];
char MessageContent3[LINE_DIM];
char TargetProductList[LINE_DIM];
char ByteString[LINE_DIM/2];
char buffer[BUF_SIZE];

FILE *output;


/* Settable Interface and Configuration (for debugging mostly) (jmw) */
int Interface = -1, Configuration = 0, AltSetting = -1;


static struct option long_options[] = {
	{"help",				no_argument, 0, 'h'},
	{"version",				no_argument, 0, 'e'},
	{"default-vendor",		required_argument, 0, 'v'},
	{"default-product",		required_argument, 0, 'p'},
	{"target-vendor",		required_argument, 0, 'V'},
	{"target-product",		required_argument, 0, 'P'},
	{"target-class",		required_argument, 0, 'C'},
	{"message-endpoint",	required_argument, 0, 'm'},
	{"message-content",		required_argument, 0, 'M'},
	{"message-content2",	required_argument, 0, '2'},
	{"message-content3",	required_argument, 0, '3'},
	{"release-delay",		required_argument, 0, 'w'},
	{"response-endpoint",	required_argument, 0, 'r'},
	{"bus-num",				required_argument, 0, 'b'},
	{"device-num",			required_argument, 0, 'g'},
	{"detach-only",			no_argument, 0, 'd'},
	{"huawei-mode",			no_argument, 0, 'H'},
	{"sierra-mode",			no_argument, 0, 'S'},
	{"sony-mode",			no_argument, 0, 'O'},
	{"qisda-mode",			no_argument, 0, 'B'},
	{"quanta-mode",			no_argument, 0, 'E'},
	{"kobil-mode",			no_argument, 0, 'T'},
	{"gct-mode",			no_argument, 0, 'G'},
	{"sequans-mode",		no_argument, 0, 'N'},
	{"mobileaction-mode",	no_argument, 0, 'A'},
	{"cisco-mode",			no_argument, 0, 'L'},
	{"blackberry-mode",		no_argument, 0, 'Z'},
	{"pantech-mode",		no_argument, 0, 'F'},
	{"need-response",		no_argument, 0, 'n'},
	{"reset-usb",			no_argument, 0, 'R'},
	{"config-file",			required_argument, 0, 'c'},
	{"verbose",				no_argument, 0, 'W'},
	{"quiet",				no_argument, 0, 'Q'},
	{"sysmode",				no_argument, 0, 'D'},
	{"no-inquire",			no_argument, 0, 'I'},
	{"stdinput",			no_argument, 0, 't'},
	{"find-mbim",			no_argument, 0, 'j'},
	{"long-config",			required_argument, 0, 'f'},
	{"check-success",		required_argument, 0, 's'},
	{"interface",			required_argument, 0, 'i'},
	{"configuration",		required_argument, 0, 'u'},
	{"altsetting",			required_argument, 0, 'a'},
	{0, 0, 0, 0}
};


void readConfigFile(const char *configFilename)
{
	ParseParamHex(configFilename, TargetVendor);
	ParseParamHex(configFilename, TargetProduct);
	ParseParamString(configFilename, TargetProductList);
	ParseParamHex(configFilename, TargetClass);
	ParseParamHex(configFilename, DefaultVendor);
	ParseParamHex(configFilename, DefaultProduct);
	ParseParamBool(configFilename, DetachStorageOnly);
	ParseParamBool(configFilename, HuaweiMode);
	ParseParamBool(configFilename, SierraMode);
	ParseParamBool(configFilename, SonyMode);
	ParseParamBool(configFilename, QisdaMode);
	ParseParamBool(configFilename, QuantaMode);
	ParseParamBool(configFilename, GCTMode);
	ParseParamBool(configFilename, KobilMode);
	ParseParamBool(configFilename, SequansMode);
	ParseParamBool(configFilename, MobileActionMode);
	ParseParamBool(configFilename, CiscoMode);
	ParseParamBool(configFilename, BlackberryMode);
	ParseParamBool(configFilename, PantechMode);
	ParseParamBool(configFilename, NoDriverLoading);
	ParseParamHex(configFilename, MessageEndpoint);
	ParseParamString(configFilename, MessageContent);
	ParseParamString(configFilename, MessageContent2);
	ParseParamString(configFilename, MessageContent3);
	ParseParamInt(configFilename, ReleaseDelay);
	ParseParamHex(configFilename, NeedResponse);
	ParseParamHex(configFilename, ResponseEndpoint);
	ParseParamHex(configFilename, ResetUSB);
	ParseParamHex(configFilename, InquireDevice);
	ParseParamInt(configFilename, CheckSuccess);
	ParseParamHex(configFilename, Interface);
	ParseParamHex(configFilename, Configuration);
	ParseParamHex(configFilename, AltSetting);

	/* TargetProductList has priority over TargetProduct */
	if (TargetProduct != -1 && TargetProductList[0] != '\0') {
		TargetProduct = -1;
		SHOW_PROGRESS(output,"Warning: TargetProductList overrides TargetProduct!\n");
	}

	config_read = 1;
}


void printConfig()
{
	if ( DefaultVendor )
		printf ("DefaultVendor=  0x%04x\n",			DefaultVendor);
	else
		fprintf (output,"DefaultVendor=  not set\n");
	if ( DefaultProduct )
		fprintf (output,"DefaultProduct= 0x%04x\n",			DefaultProduct);
	else
		fprintf (output,"DefaultProduct= not set\n");
	if ( TargetVendor )
		fprintf (output,"TargetVendor=   0x%04x\n",		TargetVendor);
	else
		fprintf (output,"TargetVendor=   not set\n");
	if ( TargetProduct > -1 )
		fprintf (output,"TargetProduct=  0x%04x\n",		TargetProduct);
	else
		fprintf (output,"TargetProduct=  not set\n");
	if ( TargetClass )
		fprintf (output,"TargetClass=    0x%02x\n",		TargetClass);
	else
		fprintf (output,"TargetClass=    not set\n");
	fprintf (output,"TargetProductList=\"%s\"\n",		TargetProductList);
	fprintf (output,"\nDetachStorageOnly=%i\n",	(int)DetachStorageOnly);
	fprintf (output,"HuaweiMode=%i\n",			(int)HuaweiMode);
	fprintf (output,"SierraMode=%i\n",			(int)SierraMode);
	fprintf (output,"SonyMode=%i\n",			(int)SonyMode);
	fprintf (output,"QisdaMode=%i\n",		(int)QisdaMode);
	fprintf (output,"QuantaMode=%i\n",		(int)QuantaMode);
	fprintf (output,"GCTMode=%i\n",			(int)GCTMode);
	fprintf (output,"KobilMode=%i\n",		(int)KobilMode);
	fprintf (output,"SequansMode=%i\n",		(int)SequansMode);
	fprintf (output,"MobileActionMode=%i\n",	(int)MobileActionMode);
	fprintf (output,"CiscoMode=%i\n",			(int)CiscoMode);
	fprintf (output,"BlackberryMode=%i\n",		(int)BlackberryMode);
	fprintf (output,"PantechMode=%i\n",			(int)PantechMode);
	if ( MessageEndpoint )
		fprintf (output,"MessageEndpoint=0x%02x\n",	MessageEndpoint);
	else
		fprintf (output,"MessageEndpoint=  not set\n");
	fprintf (output,"MessageContent=\"%s\"\n",	MessageContent);
	if ( strlen(MessageContent2) )
		fprintf (output,"MessageContent2=\"%s\"\n",	MessageContent2);
	if ( strlen(MessageContent3) )
		fprintf (output,"MessageContent3=\"%s\"\n",	MessageContent3);
	fprintf (output,"NeedResponse=%i\n",		(int)NeedResponse);
	if ( ResponseEndpoint )
		fprintf (output,"ResponseEndpoint=0x%02x\n",	ResponseEndpoint);
	else
		fprintf (output,"ResponseEndpoint= not set\n");
	if ( Interface > -1 )
		fprintf (output,"Interface=0x%02x\n",			Interface);
	if ( Configuration > 0 )
		fprintf (output,"Configuration=0x%02x\n",	Configuration);
	if ( AltSetting > -1 )
		fprintf (output,"AltSetting=0x%02x\n",	AltSetting);
	if ( InquireDevice )
		fprintf (output,"\nInquireDevice enabled (default)\n");
	else
		fprintf (output,"\nInquireDevice disabled\n");
	if ( CheckSuccess )
		fprintf (output,"Success check enabled, max. wait time %d seconds\n", CheckSuccess);
	else
		fprintf (output,"Success check disabled\n");
	if ( sysmode )
		fprintf (output,"System integration mode enabled\n");
	else
		fprintf (output,"System integration mode disabled\n");
	fprintf (output,"\n");
}


int readArguments(int argc, char **argv)
{
	int c, option_index = 0, count=0;
	char *longConfig = NULL;
	if (argc==1)
	{
		printHelp();
		printVersion();
		exit(1);
	}

	while (1)
	{
		c = getopt_long (argc, argv, "hejWQDndHSOBEGTNALZFRItv:p:V:P:C:m:M:2:3:w:r:c:i:u:a:s:f:b:g:",
						long_options, &option_index);

		/* Detect the end of the options. */
		if (c == -1)
			break;
		count++;
		switch (c)
		{
			case 'R': ResetUSB = 1; break;
			case 'v': DefaultVendor = strtol(optarg, NULL, 16); break;
			case 'p': DefaultProduct = strtol(optarg, NULL, 16); break;
			case 'V': TargetVendor = strtol(optarg, NULL, 16); break;
			case 'P': TargetProduct = strtol(optarg, NULL, 16); break;
			case 'C': TargetClass = strtol(optarg, NULL, 16); break;
			case 'm': MessageEndpoint = strtol(optarg, NULL, 16); break;
			case 'M': strncpy(MessageContent, optarg, LINE_DIM); break;
			case '2': strncpy(MessageContent2, optarg, LINE_DIM); break;
			case '3': strncpy(MessageContent3, optarg, LINE_DIM); break;
			case 'w': ReleaseDelay = strtol(optarg, NULL, 10); break;
			case 'n': NeedResponse = 1; break;
			case 'r': ResponseEndpoint = strtol(optarg, NULL, 16); break;
			case 'd': DetachStorageOnly = 1; break;
			case 'H': HuaweiMode = 1; break;
			case 'S': SierraMode = 1; break;
			case 'O': SonyMode = 1; break;
			case 'B': QisdaMode = 1; break;
			case 'E': QuantaMode = 1; break;
			case 'G': GCTMode = 1; break;
			case 'T': KobilMode = 1; break;
			case 'N': SequansMode = 1; break;
			case 'A': MobileActionMode = 1; break;
			case 'L': CiscoMode = 1; break;
			case 'Z': BlackberryMode = 1; break;
			case 'F': PantechMode = 1; break;
			case 'c': readConfigFile(optarg); break;
			case 't': readConfigFile("stdin"); break;
			case 'W': verbose = 1; show_progress = 1; count--; break;
			case 'Q': show_progress = 0; verbose = 0; count--; break;
			case 'D': sysmode = 1; InquireDevice = 0; count--; break;
			case 's': CheckSuccess = strtol(optarg, NULL, 10); count--; break;
			case 'I': InquireDevice = 0; break;
			case 'b': busnum = strtol(optarg, NULL, 10); break;
			case 'g': devnum = strtol(optarg, NULL, 10); break;

			case 'i': Interface = strtol(optarg, NULL, 16); break;
			case 'u': Configuration = strtol(optarg, NULL, 16); break;
			case 'a': AltSetting = strtol(optarg, NULL, 16); break;
			case 'j': mbim = 1; break;

			case 'f':
				longConfig = malloc(strlen(optarg)+5);
				strcpy(longConfig,"##\n");
				strcat(longConfig,optarg);
				strcat(longConfig,"\n");
				readConfigFile(longConfig);
				free(longConfig);
				break;

			case 'e':
				printVersion();
				exit(0);
				break;
			case 'h':
				printVersion();
				printHelp();
				exit(0);
				break;

			default: /* Unsupported - error message has already been printed */
				fprintf (output,"\n");
				printHelp();
				exit(1);
		}
	}

	return count;
}


int main(int argc, char **argv)
{
	int numDefaults=0, specialMode=0, sonySuccess=0;
	int currentConfig=0, defaultClass=0, interfaceClass=0;
	struct libusb_device_descriptor descriptor;
	struct libusb_config_descriptor *config;


	/* Make sure we have empty strings even if not set by config */
	TargetProductList[0] = '\0';
	MessageContent[0] = '\0';
	MessageContent2[0] = '\0';
	MessageContent3[0] = '\0';

	/* Useful for debugging during boot */
//	output=fopen("/dev/console", "w");
	output=stdout;

	signal(SIGTERM, release_usb_device);

	/*
	 * Parameter parsing, USB preparation/diagnosis, plausibility checks
	 */

	/* Check command arguments, use params instead of config file when given */
	switch (readArguments(argc, argv)) {
		case 0:						/* no argument or -W, -q or -s */
			break;
		default:					/* one or more arguments except -W, -q or -s */
			if (!config_read)		/* if arguments contain -c, the config file was already processed */
				if (verbose) fprintf(output,"Taking all parameters from the command line\n\n");
	}

	if (verbose) {
		printVersion();
		printConfig();
		SHOW_PROGRESS(output,"\n");
	}

	/* Some sanity checks. The default IDs are mandatory */
	if (!(DefaultVendor && DefaultProduct)) {
		SHOW_PROGRESS(output,"No default vendor/product ID given. Aborting.\n\n");
		exit(1);
	}
	if (strlen(MessageContent)) {
		if (strlen(MessageContent) % 2 != 0) {
			fprintf(stderr, "Error: MessageContent hex string has uneven length. Aborting.\n\n");
			exit(1);
		}
		if ( hexstr2bin(MessageContent, ByteString, strlen(MessageContent)/2) == -1) {
			fprintf(stderr, "Error: MessageContent %s\n is not a hex string. Aborting.\n\n", MessageContent);
			exit(1);
		}
	}

	if (devnum == -1) {
		searchMode = SEARCH_DEFAULT;
	} else {
		SHOW_PROGRESS(output,"Use given bus/device number: %03d/%03d ...\n", busnum, devnum);
		searchMode = SEARCH_BUSDEV;
	}

	if (show_progress)
		if (CheckSuccess && !(TargetVendor || TargetProduct > -1 || TargetProductList[0] != '\0') && !TargetClass)
			printf("Note: target parameter missing; success check limited\n");

	/* libusb initialization */
	libusb_init(&ctx);

	if (verbose)
		usb_set_debug(15);

	if (mbim) {
		printf("%d\n", findMBIMConfig(DefaultVendor, DefaultProduct, searchMode) );
		exit(0);
	}

	/* Count existing target devices, remember for success check */
	if ((TargetVendor || TargetClass) && searchMode != SEARCH_BUSDEV) {
		SHOW_PROGRESS(output,"Looking for target devices ...\n");
		search_devices(&targetDeviceCount, TargetVendor, TargetProduct, TargetProductList, TargetClass, 0, SEARCH_TARGET);
		if (targetDeviceCount) {
			SHOW_PROGRESS(output," Found devices in target mode or class (%d)\n", targetDeviceCount);
		} else
			SHOW_PROGRESS(output," No devices in target mode or class found\n");
	}

	/* Count default devices, get the last one found */
	SHOW_PROGRESS(output,"Looking for default devices ...\n");
	dev = search_devices(&numDefaults, DefaultVendor, DefaultProduct, "\0", TargetClass, Configuration, searchMode);
	if (numDefaults) {
		SHOW_PROGRESS(output," Found device in default mode, class or configuration (%d)\n", numDefaults);
	} else {
		SHOW_PROGRESS(output," No devices in default mode found. Nothing to do. Bye.\n\n");
		exit(0);
	}
	if (dev == NULL) {
		SHOW_PROGRESS(output," No bus/device match. Is device connected? Bye.\n\n");
		exit(0);
	} else {
		if (devnum == -1) {
			devnum = libusb_get_device_address(dev);
			busnum = libusb_get_bus_number(dev);
			SHOW_PROGRESS(output,"Accessing device %03d on bus %03d ...\n", devnum, busnum);
		}
		libusb_open(dev, &devh);
		if (devh == NULL) {
			SHOW_PROGRESS(output,"Error opening the device. Aborting.\n\n");
			exit(1);
		}
	}

	/* Get current configuration of default device
	 * A configuration value of -1 helps with quirky devices which have
	 * trouble determining the current configuration. We are just using the
	 * current config branch then.
	 * This affects only single-configuration devices so it's no problem.
	 * The dispatcher is using this always if no change of configuration
	 * is required for switching
	 */
	if (Configuration > -1)
		currentConfig = get_current_configuration(devh);
	else {
		SHOW_PROGRESS(output,"Skipping the check for the current configuration\n");
		currentConfig = 0;
	}

	libusb_get_device_descriptor(dev, &descriptor);
	defaultClass = descriptor.bDeviceClass;
	libusb_get_config_descriptor(dev, 0, &config);
	if (Interface == -1)
		Interface = config->interface[0].altsetting[0].bInterfaceNumber;
	SHOW_PROGRESS(output,"Using interface number %d\n", Interface);

	/* Get class of default device/interface */
	interfaceClass = get_interface_class(config, Interface);
	libusb_free_config_descriptor(config);
	if (interfaceClass == -1) {
		fprintf(stderr, "Error: getting the class of interface %d failed. Does it exist? Aborting.\n\n",Interface);
		exit(1);
	}

	if (defaultClass == 0)
		defaultClass = interfaceClass;
	else
		if (interfaceClass == 8 && defaultClass != 8) {
			/* Weird device with default class other than 0 and differing interface class */
			SHOW_PROGRESS(output,"Ambiguous Class/InterfaceClass: 0x%02x/0x08\n", defaultClass);
			defaultClass = 8;
		}

	if (strlen(MessageContent) && strncmp("55534243",MessageContent,8) == 0)
		if (defaultClass != 8) {
			fprintf(stderr, "Error: can't use storage command in MessageContent with interface %d;\n"
				"       interface class is %d, should be 8. Aborting.\n\n", Interface, defaultClass);
			exit(1);
		}

	/* Check or get endpoints */
	if (strlen(MessageContent) || InquireDevice || CiscoMode) {
		if (!MessageEndpoint)
			MessageEndpoint = find_first_bulk_output_endpoint(dev);
		if (!MessageEndpoint) {
			fprintf(stderr,"Error: message endpoint not given or found. Aborting.\n\n");
			exit(1);
		}
		if (!ResponseEndpoint)
			ResponseEndpoint = find_first_bulk_input_endpoint(dev);
		if (!ResponseEndpoint) {
			fprintf(stderr,"Error: response endpoint not given or found. Aborting.\n\n");
			exit(1);
		}
		SHOW_PROGRESS(output,"Using endpoints 0x%02x (out) and 0x%02x (in)\n", MessageEndpoint, ResponseEndpoint);
	}

	if (!MessageEndpoint || !ResponseEndpoint)
		if (InquireDevice && defaultClass == 0x08) {
			SHOW_PROGRESS(output,"Endpoints not found, skipping SCSI inquiry\n");
			InquireDevice = 0;
		}

	if (InquireDevice && show_progress) {
		if (defaultClass == 0x08) {
			SHOW_PROGRESS(output,"Inquiring device details; driver will be detached ...\n");
			detachDriver();
			if (deviceInquire() >= 0)
				InquireDevice = 2;
		} else
			SHOW_PROGRESS(output,"Not a storage device, skipping SCSI inquiry\n");
	}

	deviceDescription();
	if (show_progress) {
		printf("\nUSB description data (for identification)\n");
		printf("-------------------------\n");
		printf("Manufacturer: %s\n", imanufact);
		printf("     Product: %s\n", iproduct);
		printf("  Serial No.: %s\n", iserial);
		printf("-------------------------\n");
	}

	/* Some scenarios are exclusive, so check for unwanted combinations */
 	specialMode = DetachStorageOnly + HuaweiMode + SierraMode + SonyMode + QisdaMode + KobilMode
		+ SequansMode + MobileActionMode + CiscoMode + BlackberryMode + QuantaMode + PantechMode;
	if ( specialMode > 1 ) {
		SHOW_PROGRESS(output,"Invalid mode combination. Check your configuration. Aborting.\n\n");
		exit(1);
	}

	if ( !specialMode && !strlen(MessageContent) && AltSetting == -1 && Configuration == 0 )
		SHOW_PROGRESS(output,"Warning: no switching method given.\n");

	/*
	 * The switching actions
	 */

	if (sysmode) {
		openlog("usb_modeswitch", 0, LOG_SYSLOG);
		syslog(LOG_NOTICE, "switching device %04x:%04x on %03d/%03d", DefaultVendor, DefaultProduct, busnum, devnum);
	}

	if (DetachStorageOnly) {
		SHOW_PROGRESS(output,"Only detaching storage driver for switching ...\n");
		if (InquireDevice == 2) {
			SHOW_PROGRESS(output," Any driver was already detached for inquiry\n");
		} else {
			ret = detachDriver();
			if (ret == 2)
				SHOW_PROGRESS(output," You may want to remove the storage driver manually\n");
		}
	}

	if (HuaweiMode) {
		switchHuaweiMode();
	}
	if (SierraMode) {
		switchSierraMode();
	}
	if (GCTMode) {
		detachDriver();
		switchGCTMode();
	}
	if (QisdaMode) {
		switchQisdaMode();
	}
	if(KobilMode) {
		detachDriver();
		switchKobilMode();
	}
	if(QuantaMode) {
		switchQuantaMode();
	}
	if (SequansMode) {
		switchSequansMode();
	}
	if(MobileActionMode) {
		switchActionMode();
	}
	if(CiscoMode) {
		detachDriver();
		switchCiscoMode();
	}
	if(BlackberryMode) {
		detachDriver();
	    switchBlackberryMode();
	}
	if(PantechMode) {
		detachDriver();
		switchPantechMode();
	}
	if (SonyMode) {
		if (CheckSuccess)
			SHOW_PROGRESS(output,"Note: ignoring CheckSuccess. Separate checks for Sony mode\n");
		CheckSuccess = 0; /* separate and implied success control */
		sonySuccess = switchSonyMode();
	}

	if (strlen(MessageContent) && MessageEndpoint) {
		if (specialMode == 0) {
			if (InquireDevice != 2)
				detachDriver();
			switchSendMessage();
		} else
			SHOW_PROGRESS(output,"Warning: ignoring MessageContent. Can't combine with special mode\n");
	}

	if (Configuration > 0) {
		if (currentConfig != Configuration) {
			if (switchConfiguration()) {
				currentConfig = get_current_configuration(devh);
				if (currentConfig == Configuration) {
					SHOW_PROGRESS(output,"The configuration was set successfully\n");
				} else {
					SHOW_PROGRESS(output,"Changing the configuration has failed\n");
				}
			}
		} else {
			SHOW_PROGRESS(output,"Target configuration %d already active. Doing nothing\n", currentConfig);
		}
	}

	if (AltSetting != -1) {
		switchAltSetting();
	}

	/* No "removal" check if these are set */
	if ((Configuration > 0 || AltSetting > -1) && !ResetUSB) {
		usb_close(devh);
		devh = 0;
	}

	if (ResetUSB) {
		resetUSB();
		usb_close(devh);
		devh = 0;
	}

	if (CheckSuccess) {
		if (searchMode == SEARCH_BUSDEV && sysmode) {
			SHOW_PROGRESS(output,"Bus/dev search active, referring success check to wrapper. Bye.\n\n");
			printf("ok:busdev\n");
			goto CLOSING;
		}
		if (checkSuccess()) {
			if (sysmode) {
				if (NoDriverLoading)
					printf("ok:\n");
				else
					if (TargetProduct < 1)
						printf("ok:no_data\n");
					else
						printf("ok:%04x:%04x\n", TargetVendor, TargetProduct);
			}
		} else
			if (sysmode)
				printf("fail:\n");
	} else {
		if (SonyMode)
			if (sonySuccess) {
				if (sysmode) {
					syslog(LOG_NOTICE, "switched S.E. MD400 to modem mode");
					printf("ok:\n"); /* ACM device, no driver action */
				}
				SHOW_PROGRESS(output,"-> device should be stable now. Bye.\n\n");
			} else {
				if (sysmode)
					printf("fail:\n");
				SHOW_PROGRESS(output,"-> switching was probably not completed. Bye.\n\n");
			}
		else
			SHOW_PROGRESS(output,"-> Run lsusb to note any changes. Bye.\n\n");
	}
CLOSING:
	if (sysmode)
		closelog();
	if (devh)
		usb_close(devh);
	exit(0);
}


/* Get descriptor strings if available (identification details) */
void deviceDescription ()
{
	int ret;
	char* c;
	memset (imanufact, ' ', DESCR_MAX);
	memset (iproduct, ' ', DESCR_MAX);
	memset (iserial, ' ', DESCR_MAX);

	struct libusb_device_descriptor descriptor;
	libusb_get_device_descriptor(dev, &descriptor);

	int iManufacturer = descriptor.iManufacturer;
	int iProduct = descriptor.iProduct;
	int iSerialNumber = descriptor.iSerialNumber;

	if (iManufacturer) {
		ret = usb_get_string_simple(devh, iManufacturer, imanufact, DESCR_MAX);
		if (ret < 0)
			fprintf(stderr, "Error: could not get description string \"manufacturer\"\n");
	} else
		strcpy(imanufact, "not provided");
	c = strstr(imanufact, "    ");
	if (c)
		memset((void*)c, '\0', 1);

	if (iProduct) {
		ret = usb_get_string_simple(devh, iProduct, iproduct, DESCR_MAX);
		if (ret < 0)
			fprintf(stderr, "Error: could not get description string \"product\"\n");
	} else
		strcpy(iproduct, "not provided");
	c = strstr(iproduct, "    ");
	if (c)
		memset((void*)c, '\0', 1);

	if (iSerialNumber) {
		ret = usb_get_string_simple(devh, iSerialNumber, iserial, DESCR_MAX);
		if (ret < 0)
			fprintf(stderr, "Error: could not get description string \"serial number\"\n");
	} else
		strcpy(iserial, "not provided");
	c = strstr(iserial, "    ");
	if (c)
		memset((void*)c, '\0', 1);

}

/* Print result of SCSI command INQUIRY (identification details) */
int deviceInquire ()
{
	const unsigned char inquire_msg[] = {
	  0x55, 0x53, 0x42, 0x43, 0x12, 0x34, 0x56, 0x78,
	  0x24, 0x00, 0x00, 0x00, 0x80, 0x00, 0x06, 0x12,
	  0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	char *command;
	char data[36];
	int i, ret;

	command = malloc(31);
	if (command == NULL) {
		ret = 1;
		goto out;
	}

	memcpy(command, inquire_msg, sizeof (inquire_msg));

	ret = usb_claim_interface(devh, Interface);
	if (ret != 0) {
		SHOW_PROGRESS(output," Could not claim interface (error %d). Skipping device inquiry\n", ret);
		goto out;
	}
	usb_clear_halt(devh, MessageEndpoint);

	ret = usb_bulk_write(devh, MessageEndpoint, (char *)command, 31, 0);
	if (ret < 0) {
		SHOW_PROGRESS(output," Could not send INQUIRY message (error %d)\n", ret);
		goto out;
	}

	ret = usb_bulk_read(devh, ResponseEndpoint, data, 36, 0);
	if (ret < 0) {
		SHOW_PROGRESS(output," Could not get INQUIRY response (error %d)\n", ret);
		goto out;
	}

	i = usb_bulk_read(devh, ResponseEndpoint, command, 13, 0);

	printf("\nSCSI inquiry data (for identification)\n");
	printf("-------------------------\n");

	printf("  Vendor String: ");
	for (i = 8; i < 16; i++) printf("%c",data[i]);
	printf("\n");

	printf("   Model String: ");
	for (i = 16; i < 32; i++) printf("%c",data[i]);
	printf("\n");

	printf("Revision String: ");
	for (i = 32; i < 36; i++) printf("%c",data[i]);

	printf("\n-------------------------\n");

out:
	if (strlen(MessageContent) == 0)
		usb_clear_halt(devh, MessageEndpoint);
		usb_release_interface(devh, Interface);
	free(command);
	return ret;
}


int findMBIMConfig(int vendor, int product, int mode)
{
	struct libusb_device **devs;
	int resultConfig=0;
	int i=0, j;

	if (libusb_get_device_list(ctx, &devs) < 0) {
		perror("failed to access USB");
		return 0;
	}

	SHOW_PROGRESS(output,"Search USB devices...\n");
	while ((dev = devs[i++]) != NULL) {
		struct libusb_device_descriptor descriptor;
		libusb_get_device_descriptor(dev, &descriptor);

		if (mode == SEARCH_BUSDEV) {
			if ((libusb_get_bus_number(dev) != busnum) ||
				(libusb_get_device_address(dev) != devnum)) {
				continue;
			} else {
//				if (verbose)
//					fprintf (output,"  searching devices, found USB ID %04x:%04x\n", descriptor.idVendor, descriptor.idProduct);
				if (descriptor.idVendor != vendor)
					continue;
				if (product != descriptor.idProduct)
					continue;
			}
		}
		SHOW_PROGRESS(output,"Found device, searching for MBIM configuration...\n");

		// No check if there is only one configuration
		if (descriptor.bNumConfigurations < 2)
			return -1;

		// Checking all interfaces of all configurations
		for (j=0; j<descriptor.bNumConfigurations; j++) {
			struct libusb_config_descriptor *config;

			libusb_get_config_descriptor(dev, j, &config);
			resultConfig = config->bConfigurationValue;
			for (i=0; i<config->bNumInterfaces; i++) {
//				SHOW_PROGRESS(output,"MBIM Check: looking at ifc %d, class is %d, subclass is %d\n",
//					i,config->interface[i].altsetting[0].bInterfaceClass,config->interface[i].altsetting[0].bInterfaceSubClass);

				if ( config->interface[i].altsetting[0].bInterfaceClass == 2 )
					if ( config->interface[i].altsetting[0].bInterfaceSubClass == 0x0e ) {
						// found MBIM interface in this configuration
						libusb_free_config_descriptor(config);
						return resultConfig;
					}
			}
			libusb_free_config_descriptor(config);
		}
		return -1;
	}
	return 0;
}

void resetUSB ()
{
	int success;
	int bpoint = 0;

	if (show_progress) {
		printf("Resetting usb device ");
		fflush(stdout);
	}

	sleep( 1 );
	do {
		success = usb_reset(devh);
		if ( ((bpoint % 10) == 0) && show_progress ) {
			printf(".");
			fflush(stdout);
		}
		bpoint++;
		if (bpoint > 100)
			success = 1;
	} while (success < 0);

	if ( success ) {
		SHOW_PROGRESS(output,"\n Reset failed. Can be ignored if device switched OK.\n");
	} else
		SHOW_PROGRESS(output,"\n OK, device was reset\n");
}


int switchSendMessage ()
{
	const char* cmdHead = "55534243";
	int ret, i;
	char* msg[3];
	msg[0] = MessageContent;
	msg[1] = MessageContent2;
	msg[2] = MessageContent3;

	/* May be activated in future versions */
//	if (MessageContent2[0] != '\0' || MessageContent3[0] != '\0')
//		NeedResponse = 1;

	SHOW_PROGRESS(output,"Setting up communication with interface %d\n", Interface);
	if (InquireDevice != 2) {
		ret = usb_claim_interface(devh, Interface);
		if (ret != 0) {
			SHOW_PROGRESS(output," Could not claim interface (error %d). Skipping message sending\n", ret);
			return 0;
		}
	}
	usb_clear_halt(devh, MessageEndpoint);
	SHOW_PROGRESS(output,"Using endpoint 0x%02x for message sending ...\n", MessageEndpoint);
	if (show_progress)
		fflush(stdout);

	for (i=0; i<3; i++) {
		if ( strlen(msg[i]) == 0)
			continue;

		if ( sendMessage(msg[i], i+1) )
		goto skip;

		if (NeedResponse) {
			if ( strstr(msg[i],cmdHead) != NULL ) {
				// UFI command
				SHOW_PROGRESS(output,"Reading the response to message %d (CSW) ...\n", i+1);
				ret = read_bulk(ResponseEndpoint, ByteString, 13);
			} else {
				// Other bulk transfer
				SHOW_PROGRESS(output,"Reading the response to message %d ...\n", i+1);
				ret = read_bulk(ResponseEndpoint, ByteString, strlen(msg[i])/2 );
			}
			if (ret < 0)
				goto skip;
		}
	}

	SHOW_PROGRESS(output,"Resetting response endpoint 0x%02x\n", ResponseEndpoint);
	ret = usb_clear_halt(devh, ResponseEndpoint);
	if (ret)
		SHOW_PROGRESS(output," Could not reset endpoint (probably harmless): %d\n", ret);
	SHOW_PROGRESS(output,"Resetting message endpoint 0x%02x\n", MessageEndpoint);
	ret = usb_clear_halt(devh, MessageEndpoint);
	if (ret)
		SHOW_PROGRESS(output," Could not reset endpoint (probably harmless): %d\n", ret);
	usleep(50000);

	if (ReleaseDelay) {
		SHOW_PROGRESS(output,"Blocking the interface for %d ms before releasing ...\n", ReleaseDelay);
		usleep(ReleaseDelay*1000);
	}
	ret = usb_release_interface(devh, Interface);
	if (ret)
		goto skip;
	return 1;

skip:
	SHOW_PROGRESS(output," Device is gone, skipping any further commands\n");
	usb_close(devh);
	devh = 0;
	return 2;
}

int switchConfiguration ()
{
	int count = SWITCH_CONFIG_MAXTRIES; 
	int ret;

	SHOW_PROGRESS(output,"Changing configuration to %i ...\n", Configuration);
	while (((ret = usb_set_configuration(devh, Configuration)) < 0) && --count) {
		SHOW_PROGRESS(output," Device is busy, trying to detach kernel driver\n");
		detachDriver();
	}
	if (ret == 0 ) {
		SHOW_PROGRESS(output," OK, configuration set\n");
		return 1;
	}
	SHOW_PROGRESS(output," Setting the configuration returned error %d. Trying to continue\n", ret);
	return 0;
}


int switchAltSetting ()
{
	int ret;

	SHOW_PROGRESS(output,"Changing to alt setting %i ...\n", AltSetting);
	ret = usb_claim_interface(devh, Interface);
	ret = libusb_set_interface_alt_setting(devh, Interface, AltSetting);
	usb_release_interface(devh, Interface);
	if (ret != 0) {
		SHOW_PROGRESS(output," Changing to alt setting returned error %d. Trying to continue\n", ret);
		return 0;
	} else {
		SHOW_PROGRESS(output," OK, changed to alt setting\n");
		return 1;
	}
}


void switchHuaweiMode ()
{
	int ret;

	SHOW_PROGRESS(output,"Sending Huawei control message ...\n");
	ret = usb_control_msg(devh, USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_FEATURE, 00000001, 0, buffer, 0, 1000);
	if (ret != 0) {
		fprintf(stderr, "Error: sending Huawei control message failed (error %d). Aborting.\n\n", ret);
		exit(1);
	} else
		SHOW_PROGRESS(output," OK, Huawei control message sent\n");
}

void switchBlackberryMode ()
{
    int ret;
    SHOW_PROGRESS(output,"Sending Blackberry control message 1 ...\n");
	ret = usb_control_msg(devh, USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_IN, 0xb1, 0x0000, 0, buffer, 8, 1000);
	if (ret != 8) {
        fprintf(stderr, "Error: sending Blackberry control message 1 failed (error %d). Aborting.\n\n", ret);
		exit(1);
	} else
		SHOW_PROGRESS(output," OK, Blackberry control message 1 sent\n");
	
	SHOW_PROGRESS(output,"Sending Blackberry control message 2 ...\n");
	ret = usb_control_msg(devh, USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_IN, 0xa9, 0x000e, 0, buffer, 2, 1000);
	if (ret != 2) {
        fprintf(stderr, "Error: sending Blackberry control message 2 failed (error %d). Aborting.\n\n", ret);
		exit(1);
	} else
		SHOW_PROGRESS(output," OK, Blackberry control message 2 sent\n");
}

void switchSierraMode ()
{
	int ret;

	SHOW_PROGRESS(output,"Trying to send Sierra control message\n");
	ret = usb_control_msg(devh, USB_TYPE_VENDOR, 0x0b, 00000001, 0, buffer, 0, 1000);
	if (ret == LIBUSB_ERROR_PIPE) {
		SHOW_PROGRESS(output," communication with device stopped. May have switched, continue ...\n");
	    return;
	}
	if (ret != 0) {
		fprintf(stderr, "Error: sending Sierra control message failed (error %d). Aborting.\n\n", ret);
	    exit(1);
	} else
		SHOW_PROGRESS(output," OK, Sierra control message sent\n");
}


void switchGCTMode ()
{
	int ret;

	ret = usb_claim_interface(devh, Interface);
	if (ret != 0) {
		SHOW_PROGRESS(output," Could not claim interface (error %d). Skipping GCT sequence \n", ret);
		return;
	}

	SHOW_PROGRESS(output,"Sending GCT control message 1 ...\n");
	ret = usb_control_msg(devh, 0xa1, 0xa0, 0, Interface, buffer, 1, 1000);
	SHOW_PROGRESS(output,"Sending GCT control message 2 ...\n");
	ret = usb_control_msg(devh, 0xa1, 0xfe, 0, Interface, buffer, 1, 1000);
	SHOW_PROGRESS(output," OK, GCT control messages sent\n");
	usb_release_interface(devh, Interface);
}

void switchKobilMode()
{
	int ret;

	SHOW_PROGRESS(output,"Sending Kobil control message ...\n");
	ret = usb_control_msg(devh, USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_IN, 0x88, 0, 0, buffer, 8, 1000);
	if (ret != 0) {
		fprintf(stderr, "Error: sending Kobil control message failed (error %d). Aborting.\n\n", ret);
		exit(1);
	} else
		SHOW_PROGRESS(output," OK, Kobil control message sent\n");
}


void switchQisdaMode ()
{
	int ret;

	SHOW_PROGRESS(output,"Sending Qisda control message ...\n");
	memcpy(buffer, "\x05\x8c\x04\x08\xa0\xee\x20\x00\x5c\x01\x04\x08\x98\xcd\xea\xbf", 16);
	ret = usb_control_msg(devh, 0x40, 0x04, 00000000, 0, buffer, 16, 1000);
	if (ret != 0) {
		fprintf(stderr, "Error: sending Qisda control message failed (error %d). Aborting.\n\n", ret);
		exit(1);
	} else
		SHOW_PROGRESS(output," OK, Qisda control message sent\n");
}

void switchQuantaMode()
{
	int ret;

	SHOW_PROGRESS(output,"Sending Quanta control message ...\n");
	ret = usb_control_msg(devh, USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_IN, 0xff, 0, 0, buffer, 0, 1000);
	if (ret < 0) {
		SHOW_PROGRESS(output," Sending Quanta control message returned error %d, continue anyway ...\n", ret);
	} else {
		SHOW_PROGRESS(output," OK, Quanta control message sent\n");
	}
}

void switchPantechMode()
{
	int ret;

	SHOW_PROGRESS(output,"Sending Pantech control message ...\n");
	ret = usb_control_msg(devh, USB_TYPE_VENDOR | USB_RECIP_DEVICE, 0x70, 2, 0, buffer, 0, 1000);
	if (ret < 0) {
		SHOW_PROGRESS(output," Sending Pantech control message returned error %d, continue anyway ...\n", ret);
	} else {
		SHOW_PROGRESS(output," OK, Pantech control message sent\n");
	}
}

int switchSonyMode ()
{
	int i, found, ret;
	detachDriver();

	if (CheckSuccess) {
		printf("Note: CheckSuccess pointless with Sony mode, disabling\n");
		CheckSuccess = 0;
	}

	SHOW_PROGRESS(output,"Trying to send Sony control message\n");
	ret = usb_control_msg(devh, 0xc0, 0x11, 2, 0, buffer, 3, 100);
	if (ret < 0) {
		fprintf(stderr, "Error: sending Sony control message failed (error %d). Aborting.\n\n", ret);
		exit(1);
	} else
		SHOW_PROGRESS(output," OK, control message sent, waiting for device to return ...\n");

	usb_close(devh);
	devh = 0;

	/* Now waiting for the device to reappear */
	devnum=-1;
	busnum=-1;
	i=0;
	dev = 0;
	while ( dev == 0 && i < 30 ) {
		if ( i > 5 ) {
			dev = search_devices(&found, DefaultVendor, DefaultProduct, "\0", TargetClass, 0, SEARCH_TARGET);
		}
		if ( dev != 0 )
			break;
		sleep(1);
		if (show_progress) {
			printf("#");
			fflush(stdout);
		}
		i++;
	}
	SHOW_PROGRESS(output,"\n After %d seconds:",i);
	if ( dev ) {
		SHOW_PROGRESS(output," device came back, proceeding\n");
		libusb_open(dev, &devh);
		if (devh == 0) {
			fprintf(stderr, "Error: could not get handle on device\n");
			return 0;
		}
	} else {
		SHOW_PROGRESS(output," device still gone, cancelling\n");
		return 0;
	}
	sleep(1);

	SHOW_PROGRESS(output,"Sending Sony control message again ...\n");
	ret = usb_control_msg(devh, 0xc0, 0x11, 2, 0, buffer, 3, 100);
	if (ret < 0) {
		fprintf(stderr, "Error: sending Sony control message (2) failed (error %d)\n", ret);
		return 0;
	}
	SHOW_PROGRESS(output," OK, control message sent\n");
	return 1;
}


#define EP_OUT 0x02
#define EP_IN 0x81
#define SIZE 0x08

#define MOBILE_ACTION_READLOOP1 63
#define MOBILE_ACTION_READLOOP2 73

void switchActionMode ()
{
	int i;
	SHOW_PROGRESS(output,"Sending MobileAction control sequence ...\n");
	memcpy(buffer, "\xb0\x04\x00\x00\x02\x90\x26\x86", SIZE);
	usb_control_msg(devh, USB_TYPE_CLASS + USB_RECIP_INTERFACE, 0x09, 0x0300, 0, buffer, SIZE, 1000);
	memcpy(buffer, "\xb0\x04\x00\x00\x02\x90\x26\x86", SIZE);
	usb_control_msg(devh, USB_TYPE_CLASS + USB_RECIP_INTERFACE, 0x09, 0x0300, 0, buffer, SIZE, 1000);
	usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	memcpy(buffer, "\x37\x01\xfe\xdb\xc1\x33\x1f\x83", SIZE);
	usb_interrupt_write(devh, EP_OUT, buffer, SIZE, 1000);
	usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	memcpy(buffer, "\x37\x0e\xb5\x9d\x3b\x8a\x91\x51", SIZE);
	usb_interrupt_write(devh, EP_OUT, buffer, SIZE, 1000);
	usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	memcpy(buffer, "\x34\x87\xba\x0d\xfc\x8a\x91\x51", SIZE);
	usb_interrupt_write(devh, EP_OUT, buffer, SIZE, 1000);
	for (i=0; i < MOBILE_ACTION_READLOOP1; i++) {
		usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	}
	memcpy(buffer, "\x37\x01\xfe\xdb\xc1\x33\x1f\x83", SIZE);
	usb_interrupt_write(devh, EP_OUT, buffer, SIZE, 1000);
	usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	memcpy(buffer, "\x37\x0e\xb5\x9d\x3b\x8a\x91\x51", SIZE);
	usb_interrupt_write(devh, EP_OUT, buffer, SIZE, 1000);
	usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	memcpy(buffer, "\x34\x87\xba\x0d\xfc\x8a\x91\x51", SIZE);
	usb_interrupt_write(devh, EP_OUT, buffer, SIZE, 1000);
	for (i=0; i < MOBILE_ACTION_READLOOP2; i++) {
		usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	}
	memcpy(buffer, "\x33\x04\xfe\x00\xf4\x6c\x1f\xf0", SIZE);
	usb_interrupt_write(devh, EP_OUT, buffer, SIZE, 1000);
	usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	memcpy(buffer, "\x32\x07\xfe\xf0\x29\xb9\x3a\xf0", SIZE);
	ret = usb_interrupt_write(devh, EP_OUT, buffer, SIZE, 1000);
	usb_interrupt_read(devh, EP_IN, buffer, SIZE, 1000);
	if (ret < 0) {
		SHOW_PROGRESS(output," MobileAction control sequence did not complete\n Last error was %d\n",ret);
	} else {
		SHOW_PROGRESS(output," MobileAction control sequence complete\n");
	}
}


#define SQN_SET_DEVICE_MODE_REQUEST		0x0b
#define SQN_GET_DEVICE_MODE_REQUEST		0x0a

#define SQN_DEFAULT_DEVICE_MODE			0x00
#define SQN_MASS_STORAGE_MODE			0x01
#define SQN_CUSTOM_DEVICE_MODE			0x02

void switchSequansMode()
{
	int ret;

	SHOW_PROGRESS(output,"Sending Sequans vendor request\n");
	ret = usb_control_msg(devh, USB_TYPE_VENDOR | USB_RECIP_DEVICE, SQN_SET_DEVICE_MODE_REQUEST, SQN_CUSTOM_DEVICE_MODE, 0, buffer, 0, 1000);
	if (ret != 0) {
		fprintf(stderr, "Error: sending Sequans request failed (error %d). Aborting.\n\n", ret);
	    exit(1);
	} else
		SHOW_PROGRESS(output," OK, Sequans request was sent\n");
}

int switchCiscoMode()
{
	int ret, i;
	char* msg[11];

	SHOW_PROGRESS(output,"Preparing for sending Cisco message sequence\n");

	msg[0] = "55534243f83bcd810002000080000afd000000030000000100000000000000";
	msg[1] = "55534243984300820002000080000afd000000070000000100000000000000";
	msg[2] = "55534243984300820000000000000afd000100071000000000000000000000";
	msg[3] = "55534243984300820002000080000afd000200230000000100000000000000";
	msg[4] = "55534243984300820000000000000afd000300238200000000000000000000";
	msg[5] = "55534243984300820002000080000afd000200260000000100000000000000";
	msg[6] = "55534243984300820000000000000afd00030026c800000000000000000000";
	msg[7] = "55534243d84c04820002000080000afd000010730000000100000000000000";
	msg[8] = "55534243d84c04820002000080000afd000200240000000100000000000000";
	msg[9] = "55534243d84c04820000000000000afd000300241300000000000000000000";
	msg[10] = "55534243d84c04820000000000000afd000110732400000000000000000000";

	SHOW_PROGRESS(output,"Setting up communication with interface %d\n", Interface);
	ret = usb_claim_interface(devh, Interface);
	if (ret != 0) {
		SHOW_PROGRESS(output," Could not claim interface (error %d). Skipping message sending\n", ret);
		return 0;
	}
//	usb_clear_halt(devh, MessageEndpoint);
	if (show_progress)
		fflush(output);

	for (i=0; i<11; i++) {
		if ( sendMessage(msg[i], i+1) )
			goto skip;

		SHOW_PROGRESS(output,"Reading the response (CSW) to bulk message %d ...\n",i+1);
		ret = read_bulk(ResponseEndpoint, ByteString, 13);
		if (ret < 0)
			goto skip;
	}

	if (ReleaseDelay) {
		SHOW_PROGRESS(output,"Blocking the interface for %d ms before releasing ...\n", ReleaseDelay);
		usleep(ReleaseDelay*1000);
	}
	ret = usb_release_interface(devh, Interface);
	if (ret)
		goto skip;
	return 1;

skip:
	SHOW_PROGRESS(output,"Device returned error, skipping any further commands\n");
	usb_close(devh);
	devh = 0;
	return 2;
}

/* Detach driver
 */
int detachDriver()
{
	int ret;

#ifndef LIBUSB_HAS_GET_DRIVER_NP
	printf(" Cant't do driver detection and detaching on this platform.\n");
	return 2;
#endif

	SHOW_PROGRESS(output,"Looking for active driver ...\n");
	ret = usb_get_driver_np(devh, Interface, buffer, BUF_SIZE);
	if (ret != 0) {
		SHOW_PROGRESS(output," No driver found. Either detached before or never attached\n");
		return 1;
	}
	if (strncmp("dummy",buffer,5) == 0) {
		SHOW_PROGRESS(output," OK, driver found; name unknown, limitation of libusb1\n");
		strcpy(buffer,"unkown");
	} else {
		SHOW_PROGRESS(output," OK, driver found (\"%s\")\n", buffer);
	}

#ifndef LIBUSB_HAS_DETACH_KERNEL_DRIVER_NP
	SHOW_PROGRESS(output," Can't do driver detaching on this platform\n");
	return 2;
#endif


	ret = usb_detach_kernel_driver_np(devh, Interface);
	if (ret == 0) {
		SHOW_PROGRESS(output," OK, driver \"%s\" detached\n", buffer);
	} else
		SHOW_PROGRESS(output," Driver \"%s\" detach failed with error %d. Trying to continue\n", buffer, ret);
	return 1;
}


int sendMessage(char* message, int count)
{
	int message_length, ret;

	if (strlen(message) % 2 != 0) {
		fprintf(stderr, "Error: MessageContent %d hex string has uneven length. Skipping ...\n", count);
		return 1;
	}
	message_length = strlen(message) / 2;
	if ( hexstr2bin(message, ByteString, message_length) == -1) {
		fprintf(stderr, "Error: MessageContent %d %s\n is not a hex string. Skipping ...\n", count, MessageContent);
		return 1;
	}
	SHOW_PROGRESS(output,"Trying to send message %d to endpoint 0x%02x ...\n", count, MessageEndpoint);
	fflush(output);
	ret = write_bulk(MessageEndpoint, ByteString, message_length);
	if (ret == LIBUSB_ERROR_NO_DEVICE)
		return 1;

	return 0;
}


int checkSuccess()
{
	int i=0, ret;
	int newTargetCount, success=0;

	SHOW_PROGRESS(output,"\nChecking for mode switch (max. %d times, once per second) ...\n", CheckSuccess);
	sleep(1);

	/* If target parameters are given, don't check for vanished device
	 * Changed for Cisco AM10 where a new device is added while the install
	 * storage device stays active
	 */
	if ((TargetVendor || TargetClass) && devh) {
		usb_close(devh);
		devh = 0;
	}

	/* if target ID is not given but target class is, assign default as target;
	 * it will be needed for sysmode output
	 */
	if (!TargetVendor && TargetClass) {
		TargetVendor = DefaultVendor;
		TargetProduct = DefaultProduct;
	}

	/* devh is 0 if device vanished during command transmission or if target params were given
	 */
	if (devh)
		for (i=0; i < CheckSuccess; i++) {

			/* Test if default device still can be accessed; positive result does
			 * not necessarily mean failure
			 */
			SHOW_PROGRESS(output," Waiting for original device to vanish ...\n");

			ret = usb_claim_interface(devh, Interface);
			usb_release_interface(devh, Interface);
			if (ret < 0) {
				SHOW_PROGRESS(output," Original device can't be accessed anymore. Good.\n");
				usb_close(devh);
				devh = 0;
				break;
			}
			if (i == CheckSuccess-1) {
				SHOW_PROGRESS(output," Original device still present after the timeout\n\nMode switch most likely failed. Bye.\n\n");
			} else
				sleep(1);
		}

	if ( TargetVendor && (TargetProduct > -1 || TargetProductList[0] != '\0') ) {

		/* Recount target devices (compare with previous count) if target data is given.
		 * Target device on the same bus with higher device number is returned,
		 * description is read for syslog message
		 */
		for (i=i; i < CheckSuccess; i++) {
			SHOW_PROGRESS(output," Searching for target devices ...\n");
			dev = search_devices(&newTargetCount, TargetVendor, TargetProduct, TargetProductList, TargetClass, 0, SEARCH_TARGET);
			if (dev && (newTargetCount > targetDeviceCount)) {
				printf("\nFound target device, now opening\n");
				libusb_open(dev, &devh);
				deviceDescription();
				usb_close(devh);
				devh = 0;
				if (verbose) {
					printf("\nFound target device %03d on bus %03d\n", \
					libusb_get_device_address(dev), libusb_get_bus_number(dev));
					printf("\nTarget device description data\n");
					printf("-------------------------\n");
					printf("Manufacturer: %s\n", imanufact);
					printf("     Product: %s\n", iproduct);
					printf("  Serial No.: %s\n", iserial);
					printf("-------------------------\n");
				}
				SHOW_PROGRESS(output," Found correct target device\n\nMode switch succeeded. Bye.\n\n");
				success = 2;
				break;
			}
			if (i == CheckSuccess-1) {
				SHOW_PROGRESS(output," No new devices in target mode or class found\n\nMode switch has failed. Bye.\n\n");
			} else
				sleep(1);
		}
	} else
		/* No target data given, rely on the vanished device */
		if (!devh) {
			SHOW_PROGRESS(output," (For a better success check provide target IDs or class)\n");
			SHOW_PROGRESS(output," Original device vanished after switching\n\nMode switch most likely succeeded. Bye.\n\n");
			success = 1;
		}

	switch (success) {
		case 3: 
			if (sysmode)
				syslog(LOG_NOTICE, "switched to new device, but hit libusb1 bug");
			TargetProduct = -1;
			success = 1;
			break;
		case 2: 
			if (sysmode)
				syslog(LOG_NOTICE, "switched to %04x:%04x on %03d/%03d", TargetVendor, TargetProduct, busnum, devnum);
			success = 1;
			break;
		case 1:
			if (sysmode)
				syslog(LOG_NOTICE, "device seems to have switched");
		default:
			;
	}
	if (sysmode)
		closelog();

	return success;

}


int write_bulk(int endpoint, char *message, int length)
{
	int ret;
	ret = usb_bulk_write(devh, endpoint, message, length, 3000);
	if (ret >= 0 ) {
		SHOW_PROGRESS(output," OK, message successfully sent\n");
	} else
		if (ret == LIBUSB_ERROR_NO_DEVICE) {
			SHOW_PROGRESS(output," Device seems to have vanished right after sending. Good.\n");
		} else
			SHOW_PROGRESS(output," Sending the message returned error %d. Trying to continue\n", ret);
	return ret;

}

int read_bulk(int endpoint, char *buffer, int length)
{
	int ret;
	ret = usb_bulk_read(devh, endpoint, buffer, length, 3000);
	usb_bulk_read(devh, endpoint, buffer, 13, 100);
	if (ret >= 0 ) {
		SHOW_PROGRESS(output," OK, response successfully read (%d bytes).\n", ret);
	} else
		if (ret == LIBUSB_ERROR_NO_DEVICE) {
			SHOW_PROGRESS(output," Device seems to have vanished after reading. Good.\n");
		} else
			SHOW_PROGRESS(output," Response reading got error %d\n", ret);
	return ret;
}

void release_usb_device(int __attribute__((unused)) dummy)
{
	SHOW_PROGRESS(output,"Program cancelled by system. Bye.\n\n");
	if (devh) {
		usb_release_interface(devh, Interface);
		usb_close(devh);
	}
	if (sysmode)
		closelog();
	exit(0);
}


/* Iterates over busses and devices, counts the ones which match the given
 * parameters and returns the last one of them
*/
struct libusb_device* search_devices( int *numFound, int vendor, int product, char* productList, int targetClass, int configuration, int mode)
{
	char *listcopy=NULL, *token, buffer[2];
	int devClass;
	struct libusb_device* right_dev = NULL;
	struct libusb_device_handle *testdevh;
	struct libusb_device **devs;
	int i=0;

	/* only target class given, target vendor and product assumed unchanged */
	if ( targetClass && !(vendor || product) ) {
		vendor = DefaultVendor;
		product = DefaultProduct;
	}
	*numFound = 0;

	/* Sanity check */
	if (!vendor || (!product && productList == '\0') )
		return NULL;

	if (productList != '\0')
		listcopy = malloc(strlen(productList)+1);

	if (libusb_get_device_list(ctx, &devs) < 0) {
		perror("failed to access USB");
		return 0;
	}

	while ((dev = devs[i++]) != NULL) {
		struct libusb_device_descriptor descriptor;
		libusb_get_device_descriptor(dev, &descriptor);
		int idVendor = descriptor.idVendor;
		int idProduct = descriptor.idProduct;

		if (mode == SEARCH_BUSDEV) {
			if ((libusb_get_bus_number(dev) != busnum) ||
				(libusb_get_device_address(dev) != devnum))
				continue;
			else
				SHOW_PROGRESS(output," bus/device number matched\n");
		}

			if (verbose)
				fprintf (output,"  searching devices, found USB ID %04x:%04x\n",
						idVendor, idProduct);
			if (idVendor != vendor)
				continue;
			if (verbose)
				fprintf (output,"   found matching vendor ID\n");
			// product list given
			if ( strlen(productList) ) {
				strcpy(listcopy, productList);
				token = strtok(listcopy, ",");
				while (token != NULL) {
					if (strlen(token) != 4) {
						SHOW_PROGRESS(output,"Error: entry in product ID list has wrong length: %s. Ignoring\n", token);
						goto NextToken;
					}
					if ( hexstr2bin(token, buffer, strlen(token)/2) == -1) {
						SHOW_PROGRESS(output,"Error: entry in product ID list is not a hex string: %s. Ignoring\n", token);
						goto NextToken;
					}
					product = 0;
					product += (unsigned char)buffer[0];
					product <<= 8;
					product += (unsigned char)buffer[1];
					if (product == idProduct) {
						if (verbose)
							fprintf (output,"   found matching product ID from list\n");
						(*numFound)++;
						if (busnum == -1)
							right_dev = dev;
						else
							if (libusb_get_device_address(dev) >= devnum && libusb_get_bus_number(dev) == busnum) {
								right_dev = dev;
								TargetProduct = idProduct;
								break;
							}
					}
					NextToken:
					token = strtok(NULL, ",");
				}
			/* Product ID is given */
			} else
				if (product == idProduct) {
					SHOW_PROGRESS(output,"   found matching product ID\n");
					if (targetClass == 0 && configuration < 1) {
						(*numFound)++;
						SHOW_PROGRESS(output,"   adding device\n");
						right_dev = dev;
					} else {
						if (targetClass != 0) {
							struct libusb_device_descriptor descriptor;
							libusb_get_device_descriptor(dev, &descriptor);
							devClass = descriptor.bDeviceClass;
							struct libusb_config_descriptor *config;
							libusb_get_config_descriptor(dev, 0, &config);
							int ifaceClass = config->interface[0].altsetting[0].bInterfaceClass;
							libusb_free_config_descriptor(config);

							if (devClass == 0)
								devClass = ifaceClass;
							else
								/* Check for some quirky devices */
								if (devClass != ifaceClass)
									devClass = ifaceClass;
							if (devClass == targetClass) {
								if (verbose)
									fprintf (output,"   target class %02x matching\n", targetClass);
								if (mode == SEARCH_TARGET) {
									(*numFound)++;
									right_dev = dev;
									if (verbose)
										fprintf (output,"   adding device\n");
								} else
									if (verbose)
										fprintf (output,"   not adding device\n");
							} else {
								if (verbose)
									fprintf (output,"   target class %02x not matching\n", targetClass);
								if (mode == SEARCH_DEFAULT || mode == SEARCH_BUSDEV) {
									(*numFound)++;
									right_dev = dev;
									if (verbose)
										fprintf (output,"   adding device\n");
								}
							}
						} else {
							// check configuration (only if no target class given)
							libusb_open(dev, &testdevh);
							int testconfig = get_current_configuration(testdevh);
							if (testconfig != configuration) {
								if (verbose)
									fprintf (output,"   device configuration %d not matching parameter\n", testconfig);
								(*numFound)++;
								right_dev = dev;
								if (verbose)
									fprintf (output,"   adding device\n");
							} else
								if (verbose)
									fprintf (output,"   not adding device, target configuration already set\n");
						}
					}
				}
	}
	if (listcopy != NULL)
		free(listcopy);
	return right_dev;
}


#define USB_DIR_OUT 0x00
#define USB_DIR_IN  0x80

/* Autodetect bulk endpoints (ab) */

int find_first_bulk_output_endpoint(struct libusb_device *dev)
{
	int i;
	struct libusb_config_descriptor *config;
	libusb_get_config_descriptor(dev, 0, &config);
	const struct libusb_interface_descriptor *alt = &(config[0].interface[0].altsetting[0]);
	const struct libusb_endpoint_descriptor *ep;

	for(i=0;i < alt->bNumEndpoints;i++) {
		ep=&(alt->endpoint[i]);
		if( ( (ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_BULK) &&
		    ( (ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_OUT ) ) {
			return ep->bEndpointAddress;
		}
	}
	libusb_free_config_descriptor(config);

	return 0;
}


int find_first_bulk_input_endpoint(struct libusb_device *dev)
{
	int i;
	struct libusb_config_descriptor *config;
	libusb_get_config_descriptor(dev, 0, &config);
	const struct libusb_interface_descriptor *alt = &(config[0].interface[0].altsetting[0]);
	const struct libusb_endpoint_descriptor *ep;
	for(i=0;i < alt->bNumEndpoints;i++) {
		ep=&(alt->endpoint[i]);
		if( ( (ep->bmAttributes & USB_ENDPOINT_TYPE_MASK) == USB_ENDPOINT_TYPE_BULK) &&
		    ( (ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_DIR_IN ) ) {
			return ep->bEndpointAddress;
		}
	}
	libusb_free_config_descriptor(config);

	return 0;
}

int get_current_configuration(struct libusb_device_handle* devh)
{
	int ret;

	SHOW_PROGRESS(output,"Getting the current device configuration ...\n");
	ret = usb_control_msg(devh, USB_DIR_IN + USB_TYPE_STANDARD + USB_RECIP_DEVICE, USB_REQ_GET_CONFIGURATION, 0, 0, buffer, 1, 1000);
	if (ret < 0) {
		// There are quirky devices which fail to respond properly to this command
		fprintf(stderr, "Error getting the current configuration (error %d). Assuming configuration 1.\n", ret);
		if (Configuration) {
			fprintf(stderr, " No configuration setting possible for this device.\n");
			Configuration = 0;
		}
		return 1;
	} else {
		SHOW_PROGRESS(output," OK, got current device configuration (%d)\n", buffer[0]);
		return buffer[0];
	}
}

int get_interface_class(struct libusb_config_descriptor *cfg, int ifcNumber)
{
	int i;

	for (i=0; i<cfg->bNumInterfaces; i++) {
//		SHOW_PROGRESS(output,"Test: looking at ifc %d, class is %d\n",i,cfg->interface[i].altsetting[0].bInterfaceClass);
		if (cfg->interface[i].altsetting[0].bInterfaceNumber == ifcNumber)
			return cfg->interface[i].altsetting[0].bInterfaceClass;
	}

	return -1;
}

/* Parameter parsing */

char* ReadParseParam(const char* FileName, char *VariableName)
{
	static int numLines = 0;
	static char* ConfigBuffer[MAXLINES];

	char *VarName, *Comment=NULL, *Equal=NULL;
	char *FirstQuote, *LastQuote, *P1, *P2;
	int Line=0;
	unsigned Len=0, Pos=0;
	char Str[LINE_DIM], *token, *configPos;
	FILE *file = NULL;

	// Reading and storing input during the first call
	if (numLines==0) {
		if (strncmp(FileName,"##",2) == 0) {
			if (verbose) fprintf(output,"\nReading long config from command line\n");
			// "Embedded" configuration data
			configPos = (char*)FileName;
			token = strtok(configPos, "\n");
			strncpy(Str,token,LINE_DIM-1);
		} else {
			if (strcmp(FileName, "stdin")==0) {
				if (verbose) fprintf(output,"\nReading long config from stdin\n");
				file = stdin;
			} else {
				if (verbose) fprintf(output,"\nReading config file: %s\n", FileName);
				file=fopen(FileName, "r");
			}
			if (file==NULL) {
				fprintf(stderr, "Error: Could not find file %s\n\n", FileName);
				exit(1);
			} else {
				token = fgets(Str, LINE_DIM-1, file);
			}
		}
		while (token != NULL && numLines < MAXLINES) {
//			Line++;
			Len=strlen(Str);
			if (Len==0)
				goto NextLine;
			if (Str[Len-1]=='\n' or Str[Len-1]=='\r')
				Str[--Len]='\0';
			Equal = strchr (Str, '=');			// search for equal sign
			Pos = strcspn (Str, ";#!");			// search for comment
			Comment = (Pos==Len) ? NULL : Str+Pos;
			if (Equal==NULL or ( Comment!=NULL and Comment<=Equal))
				goto NextLine;	// Comment or irrelevant, don't save
			Len=strlen(Str)+1;
			ConfigBuffer[numLines] = malloc(Len*sizeof(char));
			strcpy(ConfigBuffer[numLines],Str);
			numLines++;
		NextLine:
			if (file == NULL) {
				token = strtok(NULL, "\n");
				if (token != NULL)
					strncpy(Str,token,LINE_DIM-1);
			} else
				token = fgets(Str, LINE_DIM-1, file);
		}
		if (file != NULL)
			fclose(file);
	}

	// Now checking for parameters
	Line=0;
	while (Line < numLines) {
		strcpy(Str,ConfigBuffer[Line]);
		Equal = strchr (Str, '=');			// search for equal sign
		*Equal++ = '\0';

		// String
		FirstQuote=strchr (Equal, '"');		// search for double quote char
		LastQuote=strrchr (Equal, '"');
		if (FirstQuote!=NULL) {
			if (LastQuote==NULL) {
				fprintf(stderr, "Error reading parameters from file %s - Missing end quote:\n%s\n", FileName, Str);
				goto Next;
			}
			*FirstQuote=*LastQuote='\0';
			Equal=FirstQuote+1;
		}

		// removes leading/trailing spaces
		Pos=strspn (Str, " \t");
		if (Pos==strlen(Str)) {
			fprintf(stderr, "Error reading parameters from file %s - Missing variable name:\n%s\n", FileName, Str);
			goto Next;		// No function name
		}
		while ((P1=strrchr(Str, ' '))!=NULL or (P2=strrchr(Str, '\t'))!=NULL)
			if (P1!=NULL) *P1='\0';
			else if (P2!=NULL) *P2='\0';
		VarName=Str+Pos;

		Pos=strspn (Equal, " \t");
		if (Pos==strlen(Equal)) {
			fprintf(stderr, "Error reading parameter from file %s - Missing value:\n%s\n", FileName, Str);
			goto Next;		// No function name
		}
		Equal+=Pos;

		if (strcmp(VarName, VariableName)==0) {		// Found it
			return Equal;
		}
	Next:
		Line++;
	}

	return NULL;
}

int hex2num(char c)
{
	if (c >= '0' && c <= '9')
	return c - '0';
	if (c >= 'a' && c <= 'f')
	return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
	return c - 'A' + 10;
	return -1;
}

int hex2byte(const char *hex)
{
	int a, b;
	a = hex2num(*hex++);
	if (a < 0)
		return -1;
	b = hex2num(*hex++);
	if (b < 0)
		return -1;
	return (a << 4) | b;
}

int hexstr2bin(const char *hex, char *buffer, int len)
{
	int i;
	int a;
	const char *ipos = hex;
	char *opos = buffer;

	for (i = 0; i < len; i++) {
	a = hex2byte(ipos);
	if (a < 0)
		return -1;
	*opos++ = a;
	ipos += 2;
	}
	return 0;
}

void printVersion()
{
	char* version = VERSION;
	printf("\n * usb_modeswitch: handle USB devices with multiple modes\n"
		" * Version %s (C) Josua Dietze 2012\n"
		" * Based on libusb0 (0.1.12 and above)\n\n"
		" ! PLEASE REPORT NEW CONFIGURATIONS !\n\n", version);
}

void printHelp()
{
	fprintf (output,"\nUsage: usb_modeswitch [<params>] [-c filename]\n\n"
	" -h, --help                    this help\n"
	" -e, --version                 print version information and exit\n"
	" -j, --find-mbim               return config no. with MBIM interface, exit\n"
	" -v, --default-vendor NUM      vendor ID of original mode (mandatory)\n"
	" -p, --default-product NUM     product ID of original mode (mandatory)\n"
	" -V, --target-vendor NUM       target mode vendor ID (optional)\n"
	" -P, --target-product NUM      target mode product ID (optional)\n"
	" -C, --target-class NUM        target mode device class (optional)\n"
	" -b, --busnum NUM              system bus number of device (for hard ID)\n"
	" -g, --devnum NUM              system device number (for hard ID)\n"
	" -m, --message-endpoint NUM    direct the message transfer there (optional)\n"
	" -M, --message-content <msg>   message to send (hex number as string)\n"
	" -2 <msg>, -3 <msg>            additional messages to send (-n recommended)\n"
	" -n, --need-response           read response to the message transfer (CSW)\n"
	" -r, --response-endpoint NUM   read response from there (optional)\n"
	" -d, --detach-only             detach the active driver, no further action\n"
	" -H, --huawei-mode             apply a special procedure\n"
	" -S, --sierra-mode             apply a special procedure\n"
	" -O, --sony-mode               apply a special procedure\n"
	" -G, --gct-mode                apply a special procedure\n"
	" -N, --sequans-mode            apply a special procedure\n"
	" -A, --mobileaction-mode       apply a special procedure\n"
	" -T, --kobil-mode              apply a special procedure\n"
	" -L, --cisco-mode              apply a special procedure\n"
	" -B, --qisda-mode              apply a special procedure\n"
	" -E, --quanta-mode             apply a special procedure\n"
	" -Z, --blackberry-mode         apply a special procedure\n"
	" -F, --pantech-mode            apply a special procedure\n"
	" -R, --reset-usb               reset the device after all other actions\n"
	" -Q, --quiet                   don't show progress or error messages\n"
	" -W, --verbose                 print all settings and debug output\n"
	" -D, --sysmode                 specific result and syslog message\n"
	" -s, --success <seconds>       switching result check with timeout\n"
	" -I, --no-inquire              do not get SCSI attributes (default on)\n\n"
	" -c, --config-file <filename>  load long configuration from file\n\n"
	" -t, --stdinput                read long configuration from stdin\n\n"
	" -f, --long-config <text>      get long configuration from string\n\n"
	" -i, --interface NUM           select initial USB interface (default 0)\n"
	" -u, --configuration NUM       select USB configuration\n"
	" -a, --altsetting NUM          select alternative USB interface setting\n\n");
}