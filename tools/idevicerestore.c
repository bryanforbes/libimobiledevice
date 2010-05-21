/*
 * idevicerestore.c
 * Restore device firmware and filesystem
 *
 * Copyright (c) 2010 Joshua Hill. All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA 
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/restore.h>

#define ASR_PORT 12345

static int quit_flag = 0;
const char operation[][35] = {
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Creating partition map",
	"Creating filesystem",
	"Restoring image",
	"Verifying restore",
	"Checking filesystems",
	"Mounting filesystems",
	"Unknown",
	"Flashing NOR",
	"Updating baseband",
	"Finalizing NAND epoch update",
	"Unknown",
	"Unknown",
	"Unknown",
	"Unknown",
	"Modifying persistent boot-args",
	"Unknown",
	"Unknown",
	"Waiting for NAND",
	"Unmounting filesystems",
	"Unknown",
	"Unknown",
	"Waiting for Device...",
	"Unknown",
	"Unknown",
	"Loading NOR data to flash"
};

int progress_msg(restored_client_t client, plist_t msg);
int send_system_data(idevice_t device, restored_client_t client, const char *filesystem);
int send_kernel_data(idevice_t device, restored_client_t client, const char *kernel);
int send_nor_data(idevice_t device, restored_client_t client);
int data_request_msg(idevice_t device, restored_client_t client, plist_t msg, const char *filesystem, const char *kernel);
int status_msg(restored_client_t client, plist_t msg);

/**
 * signal handler function for cleaning up properly
 */
static void clean_exit(int sig) {
	fprintf(stderr, "Exiting...\n");
	quit_flag++;
}

int progress_msg(restored_client_t client, plist_t msg) {
	printf("Got progress message\n");
	return 0;
}

int send_system_data(idevice_t device, restored_client_t client, const char *filesystem) {
	int i = 0;
	char buffer[0x1000];
	uint32_t recv_bytes = 0;
	memset(buffer, '\0', 0x1000);
	idevice_connection_t connection = NULL;
	idevice_error_t ret = IDEVICE_E_UNKNOWN_ERROR;
	
	for(i = 0; i < 5; i++) {
		ret = idevice_connect(device, ASR_PORT, &connection);
		if(ret == IDEVICE_E_SUCCESS)
			break;
			
		else
			sleep(1);
	}
	
	if(ret != IDEVICE_E_SUCCESS)
		return ret;
		
	memset(buffer, '\0', 0x1000);
	ret = idevice_connection_receive(connection,  buffer, 0x1000, &recv_bytes);
	if(ret != IDEVICE_E_SUCCESS) {
		idevice_disconnect(connection);
		return ret;
	}
	printf("Received %d bytes\n", recv_bytes);
	printf("%s", buffer);

	FILE* fd = fopen(filesystem, "rb");
	if(fd == NULL) {
		idevice_disconnect(connection);
		return ret;
	}
	
	fseek(fd, 0, SEEK_END);
	uint64_t len = ftell(fd);
	fseek(fd, 0, SEEK_SET);
		
	printf("Connected to ASR\n");
	plist_t dict = plist_new_dict();
	plist_dict_insert_item(dict, "FEC Slice Stride", plist_new_uint(40));
	plist_dict_insert_item(dict, "Packet Payload Size", plist_new_uint(1450));
	plist_dict_insert_item(dict, "Packets Per FEC", plist_new_uint(25));
	
	plist_t payload = plist_new_dict();
	plist_dict_insert_item(payload, "Port", plist_new_uint(1));
	plist_dict_insert_item(payload, "Size", plist_new_uint(len));
	plist_dict_insert_item(dict, "Payload", payload);
	
	plist_dict_insert_item(dict, "Stream ID", plist_new_uint(1));
	plist_dict_insert_item(dict, "Version", plist_new_uint(1));

	char* xml = NULL;
	unsigned int dict_size = 0;
	unsigned int sent_bytes = 0;
	plist_to_xml(dict, &xml, &dict_size);
	
	ret = idevice_connection_send(connection, xml, dict_size, &sent_bytes);
	if(ret != IDEVICE_E_SUCCESS) {
		idevice_disconnect(connection);
		return ret;
	}
	
	printf("Sent %d bytes\n", sent_bytes);
	printf("%s", xml);
	plist_free(dict);
	free(xml);
	
	char* command = NULL;
	do {
		memset(buffer, '\0', 0x1000);
		ret = idevice_connection_receive(connection, buffer, 0x1000, &recv_bytes);
		if(ret != IDEVICE_E_SUCCESS) {
			idevice_disconnect(connection);
			return ret;
		}
		printf("Received %d bytes\n", recv_bytes);
		printf("%s", buffer);
		
		plist_t request = NULL;
		plist_from_xml(buffer, recv_bytes, &request);
		plist_t command_node = plist_dict_get_item(request, "Command");
		if (command_node && PLIST_STRING == plist_get_node_type(command_node)) {
			plist_get_string_val(command_node, &command);
			if(!strcmp(command, "OOBData")) {
				plist_t oob_length_node = plist_dict_get_item(request, "OOB Length");
				if (!oob_length_node || PLIST_UINT != plist_get_node_type(oob_length_node)) {
					printf("Error fetching OOB Length\n");
					idevice_disconnect(connection);
					return IDEVICE_E_UNKNOWN_ERROR;
				}
				uint64_t oob_length = 0;
				plist_get_uint_val(oob_length_node, &oob_length);
				
				plist_t oob_offset_node = plist_dict_get_item(request, "OOB Offset");
				if (!oob_offset_node || PLIST_UINT != plist_get_node_type(oob_offset_node)) {
					printf("Error fetching OOB Offset\n");
					idevice_disconnect(connection);
					return IDEVICE_E_UNKNOWN_ERROR;
				}
				uint64_t oob_offset = 0;
				plist_get_uint_val(oob_offset_node, &oob_offset);
				
				char* oob_data = (char*) malloc(oob_length);
				if(oob_data == NULL) {
					printf("Out of memory\n");
					idevice_disconnect(connection);
					return IDEVICE_E_UNKNOWN_ERROR;
				}
				
				fseek(fd, oob_offset, SEEK_SET);
				if(fread(oob_data, 1, oob_length, fd) != oob_length) {
					printf("Unable to read filesystem offset\n");
					idevice_disconnect(connection);
					free(oob_data);
					return ret;
				}
				
				ret = idevice_connection_send(connection, oob_data, oob_length, &sent_bytes);
				if(sent_bytes != oob_length || ret != IDEVICE_E_SUCCESS) {
					printf("Unable to send %d bytes to asr\n", sent_bytes);
					idevice_disconnect(connection);
					free(oob_data);
					return ret;
				}
				plist_free(request);
				free(oob_data);
			}
		}
		
	} while(strcmp(command, "Payload"));
	
	fseek(fd, 0, SEEK_SET);
	char data[1450];
	for(i = len; i > 0; i -= 1450) {
		int size = 1450;
		if(i < 1450) { 
			size = i;
		}
		
		if(fread(data, 1, size, fd) != (unsigned int)size) {
			fclose(fd);
			idevice_disconnect(connection);
			printf("Error reading filesystem\n");
			return IDEVICE_E_UNKNOWN_ERROR;
		}
		
		ret = idevice_connection_send(connection, data, size, &sent_bytes);
		if(ret != IDEVICE_E_SUCCESS) {
			fclose(fd);
		}
		
		if(i % (1450*1000) == 0) {
			printf(".");
		}
	}
	
	printf("Done sending filesystem\n");
	fclose(fd);
	ret = idevice_disconnect(connection);
	return ret;
}

int send_kernel_data(idevice_t device, restored_client_t client, const char *kernel) {
	printf("Sending kernelcache\n");
	FILE* fd = fopen(kernel, "rb");
	if(fd == NULL) {
		printf("Unable to open kernelcache");
		return -1;
	}
	
	fseek(fd, 0, SEEK_END);
	uint64_t len = ftell(fd);
	fseek(fd, 0, SEEK_SET);
	
	char* kernel_data = (char*) malloc(len);
	if(kernel_data == NULL) {
		printf("Unable to allocate memory for kernel data");
		fclose(fd);
		return -1;
	}
	
	if(fread(kernel_data, 1, len, fd) != len) {
		printf("Unable to read kernel data\n");
		free(kernel_data);	
		fclose(fd);
		return -1;
	}
	fclose(fd);
	
	plist_t kernelcache_node = plist_new_data(kernel_data, len);
	
	plist_t dict = plist_new_dict();
	plist_dict_insert_item(dict, "KernelCacheFile", kernelcache_node);
	
	restored_error_t ret = restored_send(client, dict);
	if(ret != RESTORE_E_SUCCESS) {
		printf("Unable to send kernelcache data\n");
		free(kernel_data);	
		plist_free(dict);
		return -1;
	}
	
	printf("Done sending kernelcache\n");
	free(kernel_data);
	plist_free(dict);
	return 0;
}


int send_nor_data(idevice_t device, restored_client_t client) {
	printf("Not implemented\n");
	return 0;
}

int data_request_msg(idevice_t device, restored_client_t client, plist_t msg, const char *filesystem, const char *kernel) {
	plist_t datatype_node = plist_dict_get_item(msg, "DataType");
	if (datatype_node && PLIST_STRING == plist_get_node_type(datatype_node)) {
		char *datatype = NULL;
		plist_get_string_val(datatype_node, &datatype);
		if(!strcmp(datatype, "SystemImageData")) {
			send_system_data(device, client, filesystem);
		}
		else if(!strcmp(datatype, "KernelCache")) {
			send_kernel_data(device, client, kernel);
		}
		else if(!strcmp(datatype, "NORData")) {
			send_nor_data(device, client);
		}
		else {
			// Unknown DataType!!
			printf("Unknown DataType\n");
			return -1;
		}
	}
	return 0;
}

int status_msg(restored_client_t client, plist_t msg) {
	printf("Got status message\n");
	return 0;
}

static void print_usage(int argc, char **argv)
{
	char *name = NULL;
	
	name = strrchr(argv[0], '/');
	printf("Usage: %s [OPTIONS]\n", (name ? name + 1: argv[0]));
	printf("Restore firmware and filesystem to iPhone/iPod Touch.\n\n");
	printf("  -d, --debug\t\t\tenable communication debugging\n");
	printf("  -r, --recovery\t\tput device into recovery mode\n");
	printf("  -f, --filesystem FILE\t\ttarget filesystem to install onto device\n");
	printf("  -k, --kernelcache FILE\tkernelcache to install onto filesystem\n");
	printf("  -u, --uuid UUID\t\ttarget specific device by its 40-digit device UUID\n");
	printf("  -h, --help\t\t\tprints usage information\n");
	printf("\n");
}

int main(int argc, char *argv[])
{
	lockdownd_client_t lockdown_client = NULL;
	restored_client_t client = NULL;
	idevice_t phone = NULL;
	idevice_error_t ret = IDEVICE_E_UNKNOWN_ERROR;
	char *type = NULL;
	char* filesystem = NULL;
	char* kernel = NULL;
	char *uuid = NULL;
	int i;
	int return_code = 0;
	int mode = 0;
	uint64_t version = 0;

	signal(SIGINT, clean_exit);
	signal(SIGQUIT, clean_exit);
	signal(SIGTERM, clean_exit);
	signal(SIGPIPE, SIG_IGN);

	/* parse cmdline args */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")) {
			idevice_set_debug_level(1);
			continue;
		}
		else if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--uuid")) {
			i++;
			if (!argv[i] || (strlen(argv[i]) != 40)) {
				print_usage(argc, argv);
				return return_code;
			}
			strcpy(uuid, argv[i]);
			continue;
		}
		else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--recovery")) {
			mode = 1;
			continue;
		}
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			print_usage(argc, argv);
			return return_code;
		}
		else if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--filesystem")) {
			i++;
			filesystem = argv[i];
			continue;
		}
		else if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "--kernelcache")) {
			i++;
			kernel = argv[i];
			continue;
		}
		else {
			print_usage(argc, argv);
			return return_code;
		}
	}

	if (uuid != NULL) {
		ret = idevice_new(&phone, uuid);
		if (ret != IDEVICE_E_SUCCESS) {
			printf("No device found with uuid %s, is it plugged in?\n", uuid);
			return (return_code = -1);
		}
	}
	else
	{
		ret = idevice_new(&phone, NULL);
		if (ret != IDEVICE_E_SUCCESS) {
			printf("No device found, is it plugged in?\n");
			return (return_code = -1);
		}
	}

	idevice_get_uuid(phone, &uuid);

	/* check for enter recovery command */
	if (mode == 1) {
		if (LOCKDOWN_E_SUCCESS != lockdownd_client_new_with_handshake(phone, &lockdown_client, "idevicerestore")) {
			idevice_free(phone);
			return (return_code = -1);
		}

		/* run query and output information */
		printf("Telling device with uuid %s to enter recovery mode.\n", uuid);
		if(lockdownd_enter_recovery(lockdown_client) != LOCKDOWN_E_SUCCESS)
		{
			printf("ERROR: Failed to enter recovery mode.\n");
		}
		printf("Device is successfully switching to recovery mode.\n");

		lockdownd_client_free(lockdown_client);
	} else {
		if (RESTORE_E_SUCCESS != restored_client_new(phone, &client, "idevicerestore")) {
			idevice_free(phone);
			return (return_code = -1);
		}

		/* make sure device is in recovery mode */
		if (RESTORE_E_SUCCESS != restored_query_type(client, &type, &version)) {
			printf("ERROR: Device is not in restore mode. QueryType returned \"%s\"\n", type);
			return_code = -1;
			goto out;
		}

		printf("Restore protocol version is %llu.\n", version);

		/* start restored service and retrieve port */
		ret = restored_start_restore(client);
		if (ret == RESTORE_E_SUCCESS) {
			while (!quit_flag) {
				plist_t dict = NULL;
				ret = restored_receive(client, &dict);
				plist_t msgtype_node = plist_dict_get_item(dict, "MsgType");
				if (msgtype_node && PLIST_STRING == plist_get_node_type(msgtype_node)) {
					char *msgtype = NULL;
					plist_get_string_val(msgtype_node, &msgtype);
					if(!strcmp(msgtype, "ProgressMsg")) {
						ret = progress_msg(client, dict);
					
					} 
					else if(!strcmp(msgtype, "DataRequestMsg")) {
						ret = data_request_msg(phone, client, dict, filesystem, kernel);
					
					} 
					else if(!strcmp(msgtype, "StatusMsg")) {
						ret = status_msg(client, dict);
				
					} 
					else {
						printf("Received unknown message type: %s\n", msgtype);
					}
				}
			
				if (RESTORE_E_SUCCESS != ret)
					printf("Invalid return status %d\n", ret);
				
				plist_free(dict);
			}
		} else {
			printf("ERROR: Could not start restore. %d\n", ret);
		}
	}
out:
	if (type)
		free(type);
	if (client)
		restored_client_free(client);
	if (phone)
		idevice_free(phone);

	return return_code;
}

