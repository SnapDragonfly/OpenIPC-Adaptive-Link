#include <stdio.h>              // For printf, perror
#include <stdlib.h>             // For malloc, free, atoi
#include <string.h>             // For strtok, strdup
#include <unistd.h>             // For usleep
#include <pthread.h>            // For pthread functions
#include <sys/socket.h>         // For socket functions
#include <netinet/in.h>         // For sockaddr_in
#include <arpa/inet.h>          // For inet_pton
#include <stdbool.h>            // For bool, true, false
#include <sys/time.h>           // For timeval, settimeofday
#include <sys/wait.h>           // For waitpid
#include <time.h>               // For timespec, clock_gettime
#include <math.h>

#define BUFFER_SIZE 1024
#define DEFAULT_PORT 9999
#define DEFAULT_IP "10.5.0.2"
#define CONFIG_FILE "/etc/alink.conf"
#define PROFILE_FILE "/etc/txprofiles.conf"
#define MAX_PROFILES 6
#define DEFAULT_PACE_EXEC_MS 50

typedef struct {
    int rangeMin;
    int rangeMax;
    char setGI[10];
    int setMCS;
    int setFecK;
    int setFecN;
    int setBitrate;
    float setGop;
    int wfbPower;
    char ROIqp[20];
} Profile;

int prevWfbPower = -1;
float prevSetGop = -1.0;
char prevSetGI[10] = "-1";
int prevSetMCS = -1;
int prevSetFecK = -1;
int prevSetFecN = -1;
int prevSetBitrate = -1;
char prevROIqp[20] = "-1";
Profile profiles[MAX_PROFILES];

long pace_exec = DEFAULT_PACE_EXEC_MS * 1000L;
int currentProfile = -1;
int previousProfile = -2;
long prevTimeStamp = 0;
float rssi_weight = 0.5;
float snr_weight = 0.5;
int hold_fallback_mode_s = 2;
int hold_modes_down_s = 2;
int min_between_changes_ms = 100;
int request_keyframe_interval_ms = 50;
int hysteresis_percent = 10;
int baseline_value = 100;


int fallback_ms = 1000;
bool idr_every_change = false;
bool roi_focus_mode = false;


char powerCommandTemplate[100], mcsCommandTemplate[100], bitrateCommandTemplate[150], gopCommandTemplate[100], fecCommandTemplate[100], roiCommandTemplate[150], idrCommandTemplate[100], msposdCommandTemplate[512];
bool verbose_mode = false;
bool selection_busy = false;
int message_count = 0;      // Global variable for message count
bool paused = false;        // Global variable for pause state
bool time_synced = false;   // Global flag to indicate if time has been synced
int last_value_sent = 100; 
struct timespec last_exec_time; 
struct timespec last_keyframe_request_time;	// Last keyframe request command
pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;  // Mutex for message count
pthread_mutex_t pause_mutex = PTHREAD_MUTEX_INITIALIZER;  // Mutex for pause state

long get_monotonic_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

void load_config(const char* filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: Could not open configuration file: %s\n", filename);
        perror("");
        exit(EXIT_FAILURE);
    }

    char line[BUFFER_SIZE];
    while (fgets(line, sizeof(line), file)) {
        // Ignore comments (lines starting with '#')
        if (line[0] == '#') {
            continue;
        }

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");

        if (key && value) {
            if (strcmp(key, "rssi_weight") == 0) {
                rssi_weight = atof(value);
            } else if (strcmp(key, "snr_weight") == 0) {
                snr_weight = atof(value);
            } else if (strcmp(key, "hold_fallback_mode_s") == 0) {
                hold_fallback_mode_s = atoi(value);
            } else if (strcmp(key, "hold_modes_down_s") == 0) {
                hold_modes_down_s = atoi(value);
            } else if (strcmp(key, "min_between_changes_ms") == 0) {
                min_between_changes_ms = atoi(value);
			} else if (strcmp(key, "request_keyframe_interval_ms") == 0) {
                request_keyframe_interval_ms = atoi(value);
			} else if (strcmp(key, "fallback_ms") == 0) {
                fallback_ms = atoi(value);
			} else if (strcmp(key, "idr_every_change") == 0) {
                idr_every_change = atoi(value);
			} else if (strcmp(key, "hysteresis_percent") == 0) {
                hysteresis_percent = atoi(value);				
			} else if (strcmp(key, "roi_focus_mode") == 0) {
                roi_focus_mode = atoi(value);
            } else if (strcmp(key, "powerCommand") == 0) {
                strncpy(powerCommandTemplate, value, sizeof(powerCommandTemplate));
            } else if (strcmp(key, "mcsCommand") == 0) {
                strncpy(mcsCommandTemplate, value, sizeof(mcsCommandTemplate));
            } else if (strcmp(key, "bitrateCommand") == 0) {
                strncpy(bitrateCommandTemplate, value, sizeof(bitrateCommandTemplate));
            } else if (strcmp(key, "gopCommand") == 0) {
                strncpy(gopCommandTemplate, value, sizeof(gopCommandTemplate));
            } else if (strcmp(key, "fecCommand") == 0) {
                strncpy(fecCommandTemplate, value, sizeof(fecCommandTemplate));
            } else if (strcmp(key, "roiCommand") == 0) {
                strncpy(roiCommandTemplate, value, sizeof(roiCommandTemplate));
            } else if (strcmp(key, "idrCommand") == 0) {
                strncpy(idrCommandTemplate, value, sizeof(idrCommandTemplate));
			
			} else if (strcmp(key, "msposdCommand") == 0) {
                strncpy(msposdCommandTemplate, value, sizeof(msposdCommandTemplate));	
				
            } else {
                fprintf(stderr, "Warning: Unrecognized configuration key: %s\n", key);
				exit(EXIT_FAILURE);  // Exit the program with an error status
            }
        } else if (strlen(line) > 1 && line[0] != '\n') {  // ignore empty lines
            fprintf(stderr, "Error: Invalid configuration format: %s\n", line);
            exit(EXIT_FAILURE);
        }
    }

    fclose(file);
}



// Function to load profiles from profile file
void load_profiles(const char* filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Problem loading %s: ", PROFILE_FILE);
        perror(""); // Print system error message
        exit(1);
    }

    int i = 0;
    while (fscanf(file, "%d - %d %s %d %d %d %d %f %d %s",
                  &profiles[i].rangeMin, &profiles[i].rangeMax, profiles[i].setGI,
                  &profiles[i].setMCS, &profiles[i].setFecK, &profiles[i].setFecN,
                  &profiles[i].setBitrate, &profiles[i].setGop, &profiles[i].wfbPower,
                  profiles[i].ROIqp) != EOF && i < MAX_PROFILES) {
        i++;
    }

    fclose(file);
}


// Get the profile based on input value
Profile* get_profile(int input_value) {
    for (int i = 0; i < MAX_PROFILES; i++) {
        if (input_value >= profiles[i].rangeMin && input_value <= profiles[i].rangeMax) {
            return &profiles[i];
        }
    }
    return NULL;
}

// Execute system command without adding quotes
void execute_command_no_quotes(const char* command) {
    if (verbose_mode) {
        puts(command);
    }
	system(command);
	if (verbose_mode) {
		printf("Waiting %ldms\n", pace_exec / 1000);
    }
	usleep(pace_exec);

}

// Execute command, add quotes first
void execute_command(const char* command) {
    // Create a new command with quotes
    char quotedCommand[BUFFER_SIZE]; // Define a buffer for the quoted command
    snprintf(quotedCommand, sizeof(quotedCommand), "\"%s\"", command); // Add quotes around the command
    if (verbose_mode) {
        puts(quotedCommand);
    }
	system(quotedCommand);
	if (verbose_mode) {
		printf("Waiting %ldms\n", pace_exec / 1000);
    }
	usleep(pace_exec);
}


void apply_profile(Profile* profile) {
    
    char powerCommand[100];
    char mcsCommand[100];
    char bitrateCommand[150];
    char gopCommand[100];
    char fecCommand[100];
    char roiCommand[150];
    const char *idrCommand;
    char msposdCommand[512];

    idrCommand = idrCommandTemplate;

	// Calculate seconds since last change
    long currentTime = get_monotonic_time();
    long timeElapsed = currentTime - prevTimeStamp; // Time since the last change


    // Load current profile variables into local variables
    int currentWfbPower = profile->wfbPower;
    float currentSetGop = profile->setGop;
    char currentSetGI[10];
    strcpy(currentSetGI, profile->setGI);
    int currentSetMCS = profile->setMCS;
    int currentSetFecK = profile->setFecK;
    int currentSetFecN = profile->setFecN;
    int currentSetBitrate = profile->setBitrate;
    char currentROIqp[20];
    strcpy(currentROIqp, profile->ROIqp);

    // Logic to determine execution order and see if values are different
    if (currentProfile > previousProfile) {
        
        if (currentWfbPower != prevWfbPower) {
            sprintf(powerCommand, powerCommandTemplate, currentWfbPower * 50);
            execute_command(powerCommand);
            prevWfbPower = currentWfbPower;
        }
        if (currentSetGop != prevSetGop) {
            sprintf(gopCommand, gopCommandTemplate, currentSetGop);
            execute_command(gopCommand);
            prevSetGop = currentSetGop;
        }
        if (strcmp(currentSetGI, prevSetGI) != 0 || currentSetMCS != prevSetMCS) {
            sprintf(mcsCommand, mcsCommandTemplate, currentSetGI, currentSetMCS);
            execute_command(mcsCommand);
            strcpy(prevSetGI, currentSetGI);
            prevSetMCS = currentSetMCS; 
        }
        if (currentSetFecK != prevSetFecK || currentSetFecN != prevSetFecN) {
            sprintf(fecCommand, fecCommandTemplate, currentSetFecK, currentSetFecN);
            execute_command(fecCommand);
            prevSetFecK = currentSetFecK;
            prevSetFecN = currentSetFecN;
        }
        if (currentSetBitrate != prevSetBitrate) {
            sprintf(bitrateCommand, bitrateCommandTemplate, currentSetBitrate);
            execute_command(bitrateCommand);
            prevSetBitrate = currentSetBitrate; 
        }
        if (roi_focus_mode && strcmp(currentROIqp, prevROIqp) != 0) {
            sprintf(roiCommand, roiCommandTemplate, currentROIqp);
            execute_command(roiCommand);
            strcpy(prevROIqp, currentROIqp);
        }
		if (idr_every_change) {
			execute_command(idrCommand);
		}
		
    } else {
        
        if (currentSetBitrate != prevSetBitrate) {
            sprintf(bitrateCommand, bitrateCommandTemplate, currentSetBitrate);
            execute_command(bitrateCommand);
            prevSetBitrate = currentSetBitrate;
        }
        if (currentSetGop != prevSetGop) {
            sprintf(gopCommand, gopCommandTemplate, currentSetGop);
            execute_command(gopCommand);
            prevSetGop = currentSetGop;
        }
        if (strcmp(currentSetGI, prevSetGI) != 0 || currentSetMCS != prevSetMCS) {
            sprintf(mcsCommand, mcsCommandTemplate, currentSetGI, currentSetMCS);
            execute_command(mcsCommand);
            strcpy(prevSetGI, currentSetGI);
            prevSetMCS = currentSetMCS;
        }
        if (currentSetFecK != prevSetFecK || currentSetFecN != prevSetFecN) {
            sprintf(fecCommand, fecCommandTemplate, currentSetFecK, currentSetFecN);
            execute_command(fecCommand);
            prevSetFecK = currentSetFecK;
            prevSetFecN = currentSetFecN;
        }
        if (currentWfbPower != prevWfbPower) {
            sprintf(powerCommand, powerCommandTemplate, currentWfbPower * 50);
            execute_command(powerCommand);
            prevWfbPower = currentWfbPower; 
        }
        if (roi_focus_mode && strcmp(currentROIqp, prevROIqp) != 0) {
            sprintf(roiCommand, roiCommandTemplate, currentROIqp);
            execute_command(roiCommand);
            strcpy(prevROIqp, currentROIqp);
        }
		
		if (idr_every_change) {
			execute_command(idrCommand);
		}
    }
	
	
	// Display stats on msposd
	
	sprintf(msposdCommand, msposdCommandTemplate, timeElapsed, profile->setBitrate, profile->setMCS, profile->setGI,
            profile->setFecK, profile->setFecN, profile->wfbPower, profile->setGop);
	execute_command(msposdCommand);
	
	
    //sprintf(msposdCommand, "echo \"%ld s %d M:%d %s F:%d/%d P:%d G:%.1f&L30&F28 CPU:&C &Tc\" >/tmp/MSPOSD.msg",
    //        timeElapsed, profile->setBitrate, profile->setMCS, profile->setGI,
    //       profile->setFecK, profile->setFecN, profile->wfbPower, profile->setGop);
    //execute_command_no_quotes(msposdCommand); // Execute the msposd command
	
}

void value_chooses_profile(int input_value) {
    // Get the appropriate profile based on input
    Profile* selectedProfile = get_profile(input_value);
    if (selectedProfile == NULL) {
        printf("No matching profile found for input: %d\n", input_value);
		return;
    }

    // Find the index of the selected profile
    
    for (int i = 0; i < MAX_PROFILES; i++) {
        if (selectedProfile == &profiles[i]) {
            currentProfile = i;
            break;
        }
    }

    // If the previous profile is the same, do not apply changes
    if (previousProfile == currentProfile) {
        printf("No change: Link value is within same profile.\n");
		return;
    }

    // Check if a change is needed based on time constraints
    long currentTime = get_monotonic_time();
    long timeElapsed = currentTime - prevTimeStamp;

    if (previousProfile == 0 && timeElapsed <= hold_fallback_mode_s) {
        return;
    }
    if ((currentProfile - previousProfile == 1) && timeElapsed <= hold_modes_down_s) {
        return;
    }

    // Apply the selected profile
    apply_profile(selectedProfile);
	// Update previousProfile 
    previousProfile = currentProfile;
	prevTimeStamp = currentTime;
	
}

void start_selection(int rssi_score, int snr_score) {
	
	if (selection_busy) {
		return;
	}
	selection_busy = true;
	
	int value;
	struct timespec current_time;
	clock_gettime(CLOCK_MONOTONIC, &current_time);  // Use CLOCK_MONOTONIC

    long time_diff_ms = (current_time.tv_sec - last_exec_time.tv_sec) * 1000 +
                    (current_time.tv_nsec - last_exec_time.tv_nsec) / 1000000;

	if (rssi_score == 999) {	
		//Shortcut to fallback
		value_chooses_profile(999);
		last_value_sent = 999;
		baseline_value = 999;
		last_exec_time = current_time;
		selection_busy = false;
		return;
		
	} else {
		// Combine rssi and snr by weight
		float w_rssi = rssi_weight;
		float w_snr = snr_weight;
		int combined_value = floor(rssi_score * w_rssi + snr_score * w_snr);
		
		if (combined_value < 1000) {
			combined_value = 1000;
		}
		if (combined_value > 2000) {
			combined_value = 2000;
		}
		value = combined_value;		
	}

	float percent_change = fabs((float)(value - baseline_value) / baseline_value) * 100;
	if (percent_change >= hysteresis_percent) {
		// Apply profile change since the difference is significant
		if (time_diff_ms >= min_between_changes_ms) {
			printf("Qualified to request profile: %d is > %d percent different\n", value, hysteresis_percent);
			value_chooses_profile(value);
			last_value_sent = value;
			baseline_value = value;
			last_exec_time = current_time;
		} else {
			printf("Skipping profile load: value=%d, time_diff_ms=%ldms - too soon (min %dms required)\n", 
				value, time_diff_ms, min_between_changes_ms);		   
		}
	} //else {
		// No significant change, keep the same profile
		//printf("No significant change: %.2f%% (threshold: 15%%)\n", percent_change);
	//}

	selection_busy = false;
}


void special_command_message(const char *msg) {
    // Move past "special:"
    const char *cleaned_msg = msg + 8;
	const char *idrCommand = idrCommandTemplate;

    // Find the next ':'
    char *separator = strchr(cleaned_msg, ':');
    if (separator) {
        // Truncate the string at the first ':'
        *separator = '\0';
    }

    // Process the cleaned message
    if (strcmp(cleaned_msg, "pause_adaptive") == 0) {
        pthread_mutex_lock(&pause_mutex);
        paused = true;
        pthread_mutex_unlock(&pause_mutex);
        printf("Paused adaptive mode\n");
    } else if (strcmp(cleaned_msg, "resume_adaptive") == 0) {
        pthread_mutex_lock(&pause_mutex);
        paused = false;
        pthread_mutex_unlock(&pause_mutex);
        printf("Resumed adaptive mode\n");
	//} else if (strcmp(cleaned_msg, "drop_gop") == 0) {
    //    printf("Dropping GOP due to lost packets\n");
	//	system("curl localhost/api/v1/set?video0.gopSize=0.01 &");
	//	execute_channels_script(998, 1000);
    } else if (strcmp(cleaned_msg, "request_keyframe") == 0) {
        struct timespec current_time;
		clock_gettime(CLOCK_MONOTONIC, &current_time);  // Use CLOCK_MONOTONIC
		long time_diff_ms = (current_time.tv_sec - last_keyframe_request_time.tv_sec) * 1000 +
        (current_time.tv_nsec - last_keyframe_request_time.tv_nsec) / 1000000;

        if (time_diff_ms >= request_keyframe_interval_ms) {  // Check if enough time has passed
            printf("Requesting new keyframe\n");
            // Add quotes			
			char quotedCommand[BUFFER_SIZE]; // Define a buffer for the quoted command
			snprintf(quotedCommand, sizeof(quotedCommand), "\"%s\"", idrCommandTemplate); // Add quotes	around the command
			if (verbose_mode) {
				puts("Special: Requesting Keyframe");
			}
			system(quotedCommand);
            // Update the last keyframe request time
            last_keyframe_request_time = current_time;
        } else {
            printf("Skipping keyframe request: time_diff_ms=%ldms\n", time_diff_ms);
        }
    } else {
        // Handle unknown commands if needed
        printf("Unknown command: %s\n", cleaned_msg);
    }
}

void *count_messages(void *arg) {
    int local_count;
    while (1) {
        usleep(fallback_ms * 1000);
        pthread_mutex_lock(&count_mutex);
        local_count = message_count;
        message_count = 0;  //reset count
        pthread_mutex_unlock(&count_mutex);

        pthread_mutex_lock(&pause_mutex);
        if (local_count == 0 && !paused) {
            printf("No messages received in %dms, sending 999\n", fallback_ms);
            start_selection(999, 1000);
		} else {
            
			if (verbose_mode) {
				printf("Messages per %dms: %d\n", fallback_ms, local_count);
			}
        }
        pthread_mutex_unlock(&pause_mutex);
    }
    return NULL;
}

void process_message(const char *msg) {
    		
	// Declare  default local variables
    struct timeval tv;
    int transmitted_time = 0;
    int link_value_rssi = 999;
    int link_value_snr = 999;
	int recovered = 0;
    int lost = 0;
    int rssi1 = -105;
    int rssi2 = -105;
    int rssi3 = -105;
    int rssi4 = -105;

    // Copy the input string to avoid modifying the original
    char *msgCopy = strdup(msg);
    if (msgCopy == NULL) {
        perror("Failed to allocate memory");
        return;
    }

    // Use strtok to split the string by ':'
    char *token = strtok(msgCopy, ":");
    int index = 0;

    // Iterate through tokens and convert to integers
    while (token != NULL) {
        switch (index) {
            case 0:
                transmitted_time = atoi(token);
                break;
            case 1:
                link_value_rssi = atoi(token);
				break;
			case 2:
                link_value_snr = atoi(token);
                break;
            case 3:
                recovered = atoi(token);
                break;
            case 4:
                lost = atoi(token);
                break;
            case 5:
                rssi1 = atoi(token);
                break;
            case 6:
                rssi2 = atoi(token);
                break;
            case 7:
                rssi3 = atoi(token);
                break;
            case 8:
                rssi4 = atoi(token);
                break;
            default:
                // Ignore extra tokens
                break;
        }
        token = strtok(NULL, ":");
        index++;
    }

    // Print parsed values (for demonstration purposes)
   // printf("Parsed values: %d, %d, %d, %d, %d, %d, %d, %d, %d\n",
     //      transmitted_time, link_value_rssi, link_value_snr, recovered, lost, rssi1, rssi2, rssi3, rssi4);

    // Free the duplicated string
    free(msgCopy);
	
	// Only proceed with time synchronization if it hasn't been set yet
    if (!time_synced) {
        if (transmitted_time > 0) {
            tv.tv_sec = transmitted_time;
            tv.tv_usec = 0;
            if (settimeofday(&tv, NULL) == 0) {
                printf("System time synchronized with transmitted time: %ld\n", (long)transmitted_time);
                time_synced = true;
            } else {
                perror("Failed to set system time");
            }
        }
	}

    // Handle the adaptive mode pause state
    pthread_mutex_lock(&pause_mutex);
    if (!paused) {
        start_selection(link_value_rssi, link_value_snr);
    } else {
        printf("Adaptive mode paused, skipping execution\n");
    }
        pthread_mutex_unlock(&pause_mutex);

}


void print_usage() {
    printf("Usage: ./udp_server --port <port> --pace-exec <time> --verbose\n");
    printf("Options:\n");
	printf("  --ip         IP address to bind to (default: %s)\n", DEFAULT_IP);
    printf("  --port       Port to listen on (default: %d)\n", DEFAULT_PORT);
    printf("  --verbose    Enable verbose output\n");
    printf("  --pace-exec  Maj/wfb control execution pacing interval in milliseconds (default: %d ms)\n", DEFAULT_PACE_EXEC_MS);
}

int main(int argc, char *argv[]) {
    
	 // Load configuration from the config file
    load_config(CONFIG_FILE);
	// Load profiles from profile file
    load_profiles(PROFILE_FILE);
		
	int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];
    int port = DEFAULT_PORT;
    char ip[INET_ADDRSTRLEN] = DEFAULT_IP; // Default IP


    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
         } else if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            strncpy(ip, argv[++i], INET_ADDRSTRLEN);
		} else if (strcmp(argv[i], "--verbose") == 0) {
            verbose_mode = true;
        } else if (strcmp(argv[i], "--pace-exec") == 0 && i + 1 < argc) {
            int ms = atoi(argv[++i]);
            pace_exec = ms * 1000L; // Convert milliseconds to microseconds
        } else {
            print_usage();
            return 1;
        }
    }

    // Create UDP socket
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Initialize server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);

    // Bind the socket
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

   
	printf("Listening on UDP port %d, IP: %s...\n", port, ip);
    

    // Prepare counting thread
	pthread_t count_thread;
    pthread_create(&count_thread, NULL, count_messages, NULL);


 while (1) {
        // Receive a message
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                         (struct sockaddr *)&client_addr, &client_addr_len);
        if (n < 0) {
            perror("recvfrom failed");
            break;
        }
		
		// Increment message count
			pthread_mutex_lock(&count_mutex);
			message_count++;
			pthread_mutex_unlock(&count_mutex);

        // Null-terminate the received data
        buffer[n] = '\0';

        // Extract the length of the message (first 4 bytes)
        uint32_t msg_length;
        memcpy(&msg_length, buffer, sizeof(msg_length));
        msg_length = ntohl(msg_length); // Convert from network to host byte order

        // Print the message length and content
        if (verbose_mode) {
			printf("Received message (%u bytes): %s\n", msg_length, buffer + sizeof(msg_length));
					
		}
		
		// Strip length off the start of the message
		char *message = buffer + sizeof(uint32_t);
		//See if it's special otherwise just process it
		if (strncmp(message, "special:", 8) == 0) {
			special_command_message(message);
		} else {
			process_message(message);
		}	
		
    }

    // Close the socket
    close(sockfd);
    return 0;
}
