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
#include <ctype.h>
#include <limits.h>

#define MAX_COMMAND_SIZE 256
#define BUFFER_SIZE 1024
#define DEFAULT_PORT 9999
#define DEFAULT_IP "10.5.0.10"
#define CONFIG_FILE "/etc/alink.conf"
#define PROFILE_FILE "/etc/txprofiles.conf"
#define MAX_PROFILES 20
#define DEFAULT_PACE_EXEC_MS 50

#define min(a, b) ((a) < (b) ? (a) : (b))

// Profile struct
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
	int bandwidth;
	int setQpDelta;
} Profile;

Profile profiles[MAX_PROFILES];

// osd2udp struct
typedef struct {
    int udp_out_sock;
    char udp_out_ip[INET_ADDRSTRLEN];
    int udp_out_port;
} osd_udp_config_t;

// OSD strings
char global_profile_osd[48] = "initializing...";
char global_profile_fec_osd[16] = "0/0";
char global_regular_osd[64] = "&L%d0&F%d&B &C tx&Wc";
char global_gs_stats_osd[64] = "waiting for gs.";
char global_extra_stats_osd[256] = "initializing...";
char global_score_related_osd[64] = "initializing...";

int osd_level = 4;


int x_res = 1920;
int y_res = 1080;
int global_fps = 120;
int total_pixels = 2073600;
int set_osd_font_size = 20;
int set_osd_colour = 7;
float multiply_font_size_by = 0.5;

int num_antennas = 0;
int num_antennas_drone = 0;
int noise_pnlty = 0;
int fec_change = 0;
int prev_fec_change = 0;

int prevWfbPower = -1;
float prevSetGop = -1.0;
int prevBandwidth = -20;
char prevSetGI[10] = "-1";
int prevSetMCS = -1;
char prevROIqp[20] = "-1";
int prevSetFecK = -1;
int prevSetFecN = -1;
int prevSetBitrate = -1;
int prevDivideFpsBy = -1;
int prevFPS = -1;
int prevQpDelta = -100;
int old_bitrate = -1;
int old_fec_k = -1;
int old_fec_n = -1;


int tx_factor = 50;  // Default tx power factor 50 (most cards)
int ldpc_tx = 1;
int stbc = 1;
long pace_exec = DEFAULT_PACE_EXEC_MS * 1000L;
int currentProfile = -1;
int previousProfile = -2;
long prevTimeStamp = 0;
bool allow_set_power = 1;
float rssi_weight = 0.5;
float snr_weight = 0.5;
int hold_fallback_mode_s = 2;
int hold_modes_down_s = 2;
int min_between_changes_ms = 100;
int request_keyframe_interval_ms = 50;
bool allow_request_keyframe = 1;
bool allow_rq_kf_by_tx_d = 1;
int check_xtx_period_ms = 500;
int hysteresis_percent = 15;
int hysteresis_percent_down = 5;
int baseline_value = 100;
float smoothing_factor = 0.5;
float smoothing_factor_down = 0.8;
float smoothed_combined_value = 1500;
bool limitFPS = 1;
bool get_card_info_from_yaml = false;

bool allow_dynamic_fec = 1;
bool fec_k_adjust = 0;

int fallback_ms = 1000;
bool idr_every_change = false;
bool roi_focus_mode = false;

char fpsCommandTemplate[150], powerCommandTemplate[100], qpDeltaCommandTemplate[150], mcsCommandTemplate[100], bitrateCommandTemplate[150], gopCommandTemplate[100], fecCommandTemplate[100], roiCommandTemplate[150], idrCommandTemplate[100];
bool verbose_mode = false;
bool selection_busy = false;
bool initialized_by_first_message = false;
int message_count = 0;      // Global variable for message count
bool paused = false;        // Global variable for pause state
bool time_synced = false;   // Global flag to indicate if time has been synced
int last_value_sent = 100;
struct timespec last_exec_time;
struct timespec last_keyframe_request_time;	// Last keyframe request command
pthread_mutex_t count_mutex = PTHREAD_MUTEX_INITIALIZER;  // Mutex for message count
pthread_mutex_t pause_mutex = PTHREAD_MUTEX_INITIALIZER;  // Mutex for pause state

#define MAX_CODES 5       // Maximum number of unique keyframe requests to track
#define CODE_LENGTH 8     // Max length of each unique code
#define EXPIRY_TIME_MS 1000 // Code expiry time in milliseconds

int total_keyframe_requests = 0;
int total_keyframe_requests_xtx = 0;
long global_total_tx_dropped = 0;


volatile int weak_antenna_detected = 0;
// monitor drone antenna rssi
void *parse_rssi_thread(void *arg) {
    const char *FIFO_PATH = "/tmp/wfb_rx.log";
    const int MAX_LINE = 512;
    const int NUM_ANTENNAS = 4; //max
    const int HISTORY_SIZE = 20;
    const int RSSI_THRESHOLD = 20;  // to trigger flag

    FILE *fp = fopen(FIFO_PATH, "r");
    if (!fp) {
        perror("Failed to open FIFO, not tracking local wfb_rx tunnel stats\nIf you want to track these, add the logic to your wifibroadcast script");
        pthread_exit(NULL);
    }

    int rssi_history[NUM_ANTENNAS][HISTORY_SIZE];
    int rssi_index[NUM_ANTENNAS];
    int rssi_avg[NUM_ANTENNAS];
    int rssi_count[NUM_ANTENNAS];

    // Initialize arrays
    for (int i = 0; i < NUM_ANTENNAS; i++) {
        rssi_index[i] = 0;
        rssi_avg[i] = 0;
        rssi_count[i] = 0;
        for (int j = 0; j < HISTORY_SIZE; j++) {
            rssi_history[i][j] = 0;
        }
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "RX_ANT")) {
            char freq_mcs_band[64], colon_values[128];
            int antenna, timestamp;
            
            if (sscanf(line, "%d RX_ANT %63s %d %127[^\n]", &timestamp, freq_mcs_band, &antenna, colon_values) == 4) {
                if (antenna < 0 || antenna >= NUM_ANTENNAS) continue; // Ignore invalid antennas
				if (antenna >= num_antennas_drone) {
					num_antennas_drone = antenna + 1;  // Update global
				}
                
                char *token;
                int token_count = 0, rssi = 0;
                token = strtok(colon_values, ":");
                while (token) {
                    if (++token_count == 3) {  // RSSI is the third field
                        rssi = atoi(token);
                        break;
                    }
                    token = strtok(NULL, ":");
                }

                // Store RSSI in history buffer
                rssi_history[antenna][rssi_index[antenna] % HISTORY_SIZE] = rssi;
                rssi_index[antenna]++;
                rssi_count[antenna]++;

                // Compute moving average
                int sum = 0, count = rssi_count[antenna] < HISTORY_SIZE ? rssi_count[antenna] : HISTORY_SIZE;
                for (int i = 0; i < count; i++) {
                    sum += rssi_history[antenna][i];
                }
                rssi_avg[antenna] = sum / count;

                // Detect weak antenna
                int min_rssi = INT_MAX, max_rssi = INT_MIN;
                for (int i = 0; i < NUM_ANTENNAS; i++) {
                    if (rssi_count[i] > 0) {
                        if (rssi_avg[i] < min_rssi) min_rssi = rssi_avg[i];
                        if (rssi_avg[i] > max_rssi) max_rssi = rssi_avg[i];
                    }
                }
                if (max_rssi - min_rssi >= RSSI_THRESHOLD) {
                    weak_antenna_detected = 1;
                } else {
                    weak_antenna_detected = 0;
                }
            }
        }
    }

    fclose(fp);
    pthread_exit(NULL);
}

void error_to_osd(const char *message) {
    const char *prefix = "&L50&F30 ";
    char full_message[128];

    snprintf(full_message, sizeof(full_message), "%s%s", prefix, message);

    FILE *file = fopen("/tmp/MSPOSD.msg", "w");
    if (file == NULL) {
        perror("Error opening /tmp/MSPOSD.msg");
        return;
    }

    if (fwrite(full_message, sizeof(char), strlen(full_message), file) != strlen(full_message)) {
        perror("Error writing to /tmp/MSPOSD.msg");
    }

    fclose(file);
}


// Struct to store each keyframe request code and its timestamp
typedef struct {
    char code[CODE_LENGTH];
    struct timespec timestamp;
} KeyframeRequest;

// Static array of keyframe requests
static KeyframeRequest keyframe_request_codes[MAX_CODES];
static int num_keyframe_requests = 0;  // Track the number of stored keyframe requests


long get_monotonic_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

int get_resolution() {
	
	char resolution[32];

    // Execute system command to get resolution
    FILE *fp = popen("cli --get .video0.size", "r");
    if (fp == NULL) {
        printf("Failed to run get resolution command\n");
        return 1;
    }

    if (fgets(resolution, sizeof(resolution) - 1, fp) == NULL) {
		printf("fgets failed\n");
	}
	
	pclose(fp);

    // Parse the resolution in the format <x_res>x<y_res>
    if (sscanf(resolution, "%dx%d", &x_res, &y_res) != 2) {
        printf("Failed to parse resolution\n");
        return 1;
    }

	printf("Video Size: %dx%d\n", x_res, y_res);
    return 0;


}

void load_config(const char* filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: Could not open configuration file: %s\n", filename);
        perror("");
        error_to_osd("Adaptive-Link: Check/update /etc/alink.conf");
        exit(EXIT_FAILURE);
    }

    char line[BUFFER_SIZE];
    while (fgets(line, sizeof(line), file)) {
        // Ignore comments (lines starting with '#')
        if (line[0] == '#')
            continue;

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");

        if (key && value) {
            if (strcmp(key, "allow_set_power") == 0) {
                allow_set_power = atoi(value);
            } else if (strcmp(key, "rssi_weight") == 0) {
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
            } else if (strcmp(key, "allow_request_keyframe") == 0) {
                allow_request_keyframe = atoi(value);
			} else if (strcmp(key, "get_card_info_from_yaml") == 0) {
                get_card_info_from_yaml = atoi(value);	
						
			} else if (strcmp(key, "allow_dynamic_fec") == 0) {
                allow_dynamic_fec = atoi(value);
			
			} else if (strcmp(key, "fec_k_adjust") == 0) {
                fec_k_adjust = atoi(value);
				
            } else if (strcmp(key, "allow_rq_kf_by_tx_d") == 0) {
                allow_rq_kf_by_tx_d = atoi(value);
            } else if (strcmp(key, "hysteresis_percent") == 0) {
                hysteresis_percent = atoi(value);
            } else if (strcmp(key, "hysteresis_percent_down") == 0) {
                hysteresis_percent_down = atoi(value);
            } else if (strcmp(key, "exp_smoothing_factor") == 0) {
                smoothing_factor = atof(value);
            } else if (strcmp(key, "exp_smoothing_factor_down") == 0) {
                smoothing_factor_down = atof(value);
            } else if (strcmp(key, "roi_focus_mode") == 0) {
                roi_focus_mode = atoi(value);
            
            } else if (strcmp(key, "allow_spike_fix_fps") == 0) {
                limitFPS = atoi(value);
         
            } else if (strcmp(key, "osd_level") == 0) {
                osd_level = atoi(value);
            } else if (strcmp(key, "multiply_font_size_by") == 0) {
                multiply_font_size_by = atof(value);
            } else if (strcmp(key, "check_xtx_period_ms") == 0) {
                check_xtx_period_ms = atoi(value);
            }
            // New keys for command templates:
            else if (strcmp(key, "powerCommandTemplate") == 0) {
                strncpy(powerCommandTemplate, value, sizeof(powerCommandTemplate));
            } else if (strcmp(key, "fpsCommandTemplate") == 0) {
                strncpy(fpsCommandTemplate, value, sizeof(fpsCommandTemplate));
            } else if (strcmp(key, "qpDeltaCommandTemplate") == 0) {
                strncpy(qpDeltaCommandTemplate, value, sizeof(qpDeltaCommandTemplate));
            } else if (strcmp(key, "mcsCommandTemplate") == 0) {
                strncpy(mcsCommandTemplate, value, sizeof(mcsCommandTemplate));
            } else if (strcmp(key, "bitrateCommandTemplate") == 0) {
                strncpy(bitrateCommandTemplate, value, sizeof(bitrateCommandTemplate));
            } else if (strcmp(key, "gopCommandTemplate") == 0) {
                strncpy(gopCommandTemplate, value, sizeof(gopCommandTemplate));
            } else if (strcmp(key, "fecCommandTemplate") == 0) {
                strncpy(fecCommandTemplate, value, sizeof(fecCommandTemplate));
            } else if (strcmp(key, "roiCommandTemplate") == 0) {
                strncpy(roiCommandTemplate, value, sizeof(roiCommandTemplate));
            } else if (strcmp(key, "idrCommandTemplate") == 0) {
                strncpy(idrCommandTemplate, value, sizeof(idrCommandTemplate));
            } else if (strcmp(key, "customOSD") == 0) {
                strncpy(global_regular_osd, value, sizeof(global_regular_osd));
            } else {
                fprintf(stderr, "Warning: Unrecognized configuration key: %s\n", key);
                error_to_osd("Adaptive-Link: Check/update /etc/alink.conf");
                exit(EXIT_FAILURE);
            }
        } else if (strlen(line) > 1 && line[0] != '\n') {  // ignore empty lines
            fprintf(stderr, "Error: Invalid configuration format: %s\n", line);
            error_to_osd("Adaptive-Link: Check/update /etc/alink.conf");
            exit(EXIT_FAILURE);
        }
    }

    fclose(file);
}



void trim_whitespace(char *str) {
    char *end;
    
    // Trim leading spaces
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) return; // Empty string

    // Trim trailing spaces
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    // Null-terminate the trimmed string
    *(end + 1) = '\0';
}

void normalize_whitespace(char *str) {
    char *src = str, *dst = str;
    int in_space = 0;

    while (*src) {
        if (isspace((unsigned char)*src)) {
            if (!in_space) {
                *dst++ = ' ';  // Replace any whitespace sequence with a single space
                in_space = 1;
            }
        } else {
            *dst++ = *src;
            in_space = 0;
        }
        src++;
    }
    *dst = '\0';  // Null-terminate the cleaned string
}

void load_profiles(const char* filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Problem loading %s: ", filename);
		error_to_osd("Adaptive-Link: Check /etc/txprofiles.conf");

		
        perror("");
        exit(1);
    }

    char line[256];
    int i = 0;

    while (fgets(line, sizeof(line), file) && i < MAX_PROFILES) {
        // Remove comments
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';

        // Trim and normalize spaces
        trim_whitespace(line);
        normalize_whitespace(line);

        // Skip empty lines
        if (*line == '\0') continue;

        // Parse the cleaned line
        if (sscanf(line, "%d - %d %15s %d %d %d %d %f %d %15s %d %d",
                   &profiles[i].rangeMin, &profiles[i].rangeMax, profiles[i].setGI,
                   &profiles[i].setMCS, &profiles[i].setFecK, &profiles[i].setFecN,
                   &profiles[i].setBitrate, &profiles[i].setGop, &profiles[i].wfbPower,
                   profiles[i].ROIqp, &profiles[i].bandwidth, &profiles[i].setQpDelta) == 12) {
            i++;
        } else {
            fprintf(stderr, "Malformed line ignored: %s\n", line);
						
        }
    }

    fclose(file);
}

int check_module_loaded(const char *module_name) {
    FILE *fp = fopen("/proc/modules", "r");
    if (!fp) {
        perror("Failed to open /proc/modules");
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, module_name, strlen(module_name)) == 0) {
            fclose(fp);
            return 1; // Found the module
        }
    }

    fclose(fp);
    return 0; // Not found
}

void load_from_vtx_info_yaml() {
    char command1[] = "yaml-cli -i /etc/wfb.yaml -g .broadcast.ldpc";
    char command2[] = "yaml-cli -i /etc/wfb.yaml -g .broadcast.stbc";
    
    char buffer[128]; // Buffer to store command output
    FILE *pipe;
    
    // Retrieve ldpc_tx value
    pipe = popen(command1, "r");
    if (pipe == NULL) {
        fprintf(stderr, "Failed to run yaml reader for ldpc_tx\n");
        return;
    }
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        ldpc_tx = atoi(buffer);
    }
    pclose(pipe);

    // Retrieve stbc value
    pipe = popen(command2, "r");
    if (pipe == NULL) {
        fprintf(stderr, "Failed to run yaml reader for stbc\n");
        return;
    }
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        stbc = atoi(buffer);
    }
    pclose(pipe);
}

void determine_tx_power_equation() {
    if (check_module_loaded("88XXau")) {
        tx_factor = -100;
		printf("Found 88XXau card\n");
    } else {
        tx_factor = 50;
		printf("Did not find 88XXau\n");
    }
}

// Function to read fps from majestic.yaml
int get_video_fps() {
    char command[] = "cli --get .video0.fps";
    char buffer[128]; // Buffer to store command output
    FILE *pipe;
    int fps = 0;

    // Open a pipe to execute the command
    pipe = popen(command, "r");
    if (pipe == NULL) {
        fprintf(stderr, "Failed to run cli --get .video0.fps\n");
        return -1; // Return an error code
    }

    // Read the output from the command
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        // Convert the output string to an integer
        fps = atoi(buffer);
    }

    // Close the pipe
    pclose(pipe);

    return fps;
}

// Function to setup roi in majestic.yaml based on resolution
int setup_roi() {
    
    FILE *fp;  // Declare the FILE pointer before using it

	
    // Round x_res and y_res to nearest multiples of 32
    int rounded_x_res = floor(x_res / 32) * 32;
    int rounded_y_res = floor(y_res / 32) * 32;

    // ROI calculation with additional condition
    int roi_height, start_roi_y;
    if (rounded_y_res != y_res) {
        roi_height = rounded_y_res - 32;
        start_roi_y = 32;
    } else {
        roi_height = rounded_y_res;
        start_roi_y = y_res - rounded_y_res;
    }

    // Make rois 32 lower for clear stats, make total roi 32 less
    roi_height = roi_height - 32;
    start_roi_y = start_roi_y + 32;

    // Calculate edge_roi_width and next_roi_width as multiples of 32
    int edge_roi_width = floor(rounded_x_res / 8 / 32) * 32;
    int next_roi_width = (floor(rounded_x_res / 8 / 32) * 32) + 32;

    int coord0 = 0;
    int coord1 = edge_roi_width;
    int coord2 = x_res - edge_roi_width - next_roi_width;
    int coord3 = x_res - edge_roi_width;

    // Format ROI definition as a string
    char roi_define[256];
    snprintf(roi_define, sizeof(roi_define), "%dx%dx%dx%d,%dx%dx%dx%d,%dx%dx%dx%d,%dx%dx%dx%d",
             coord0, start_roi_y, edge_roi_width, roi_height,
             coord1, start_roi_y, next_roi_width, roi_height,
             coord2, start_roi_y, next_roi_width, roi_height,
             coord3, start_roi_y, edge_roi_width, roi_height);

    // Prepare the command to set ROI
    char command[512];
    snprintf(command, sizeof(command), "cli --set .fpv.roiRect %s", roi_define);

    // Check if .fpv.enabled is set
    char enabled_status[16];
    fp = popen("cli --get .fpv.enabled", "r");
    if (fp == NULL) {
        printf("Failed to run command\n");
        return 1;
    }

    if (fgets(enabled_status, sizeof(enabled_status) - 1, fp) == NULL) {
		printf("fgets failed\n");
	} 

    // Trim newline character
    enabled_status[strcspn(enabled_status, "\n")] = 0;

    // Check if enabled_status is "true" or "false"
    if (strcmp(enabled_status, "true") != 0 && strcmp(enabled_status, "false") != 0) {
		if (system("cli --set .fpv.enabled true") != 0) { printf("problem with reading fpv.enabled status\n"); }
    }

    // Run the command to set ROI
	if (system(command) != 0) { printf("set ROI command failed\n"); }

    // Check if .fpv.roiQp is set correctly
    char roi_qp_status[32];
    fp = popen("cli --get .fpv.roiQp", "r");
    if (fp == NULL) {
        printf("Failed to run command\n");
        return 1;
    }

	if (fgets(roi_qp_status, sizeof(roi_qp_status) - 1, fp) == NULL) { printf("fgets failed\n"); }
    pclose(fp);

    // Trim newline character
    roi_qp_status[strcspn(roi_qp_status, "\n")] = 0;

    // Check for four integers separated by commas
    int num_count = 0;
    char *token = strtok(roi_qp_status, ",");
    while (token != NULL) {
        num_count++;
        token = strtok(NULL, ",");
    }

    if (num_count != 4) {
		if (system("cli --set .fpv.roiQp 0,0,0,0") != 0) { printf("Command failed\n"); }
    }

    return 0;
}

void read_wfb_tx_cmd_output(int *k, int *n, int *stbc, int *ldpc, int *short_gi, int *actual_bandwidth, int *mcs_index, int *vht_mode, int *vht_nss) {
    char buffer[256];
    FILE *fp;
    
    // Run first command
    fp = popen("wfb_tx_cmd 8000 get_fec", "r");
    if (fp == NULL) {
        perror("Failed to run wfb_tx_cmd command");
        return;
    }
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        if (sscanf(buffer, "k=%d", k) == 1) continue;
        if (sscanf(buffer, "n=%d", n) == 1) continue;
    }
    pclose(fp);
    
    // Run second command
    fp = popen("wfb_tx_cmd 8000 get_radio", "r");
    if (fp == NULL) {
        perror("Failed to run wfb_tx_cmd command");
        return;
    }
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        if (sscanf(buffer, "stbc=%d", stbc) == 1) continue;
        if (sscanf(buffer, "ldpc=%d", ldpc) == 1) continue;
        if (sscanf(buffer, "short_gi=%d", short_gi) == 1) continue;
        if (sscanf(buffer, "bandwidth=%d", actual_bandwidth) == 1) continue;
        if (sscanf(buffer, "mcs_index=%d", mcs_index) == 1) continue;
        if (sscanf(buffer, "vht_mode=%d", vht_mode) == 1) continue;
        if (sscanf(buffer, "vht_nss=%d", vht_nss) == 1) continue;
    }
    pclose(fp);
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
	
	if (system(command) != 0) { printf("Command failed: %s\n", command); }
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
	if (system(quotedCommand) != 0) { printf("Command failed: %s\n", quotedCommand); }
	if (verbose_mode) {
		printf("Waiting %ldms\n", pace_exec / 1000);
    }
	usleep(pace_exec);
}

// Replaces the first occurrence of a placeholder (e.g. "{name}") in 'str' with 'value'
void replace_placeholder(char *str, const char *placeholder, const char *value) {
    char buffer[MAX_COMMAND_SIZE];
    char *pos = strstr(str, placeholder);
    if (!pos)
        return; // placeholder not found
    size_t prefix_len = pos - str;
    buffer[0] = '\0';
    strncat(buffer, str, prefix_len);
    strncat(buffer, value, sizeof(buffer) - strlen(buffer) - 1);
    strncat(buffer, pos + strlen(placeholder), sizeof(buffer) - strlen(buffer) - 1);
    strncpy(str, buffer, MAX_COMMAND_SIZE);
    str[MAX_COMMAND_SIZE-1] = '\0';
}

// Formats a command by replacing named placeholders with the provided values.
// 'count' is the number of keys/values, and keys/values are provided in parallel arrays.
void format_command(char *dest, size_t dest_size, const char *template,
                    int count, const char **keys, const char **values) {
    char temp[MAX_COMMAND_SIZE];
    strncpy(temp, template, sizeof(temp));
    temp[sizeof(temp)-1] = '\0';
    char placeholder[64];
    for (int i = 0; i < count; i++) {
        snprintf(placeholder, sizeof(placeholder), "{%s}", keys[i]);
        replace_placeholder(temp, placeholder, values[i]);
    }
    strncpy(dest, temp, dest_size);
    dest[dest_size-1] = '\0';
}



void manage_fec_and_bitrate(int new_fec_k, int new_fec_n, int new_bitrate) {
    char fecCommand[MAX_COMMAND_SIZE];
    char bitrateCommand[MAX_COMMAND_SIZE];

    // Adjust fec and bitrate based on fec_change (if applicable)
    if (allow_dynamic_fec && fec_change > 0 && fec_change <= 5) {
        float denominators[] = { 1, 1.11111, 1.25, 1.42, 1.66667, 2.0 };
        float denominator = denominators[fec_change];
        new_bitrate = (int)(new_bitrate / denominator);
        // divide k or multiply n depending on fec_k_adjust option
		(fec_k_adjust) ? (new_fec_k /= denominator) : (new_fec_n *= denominator);

    }
    
    // Update the global FEC OSD regardless of order.
    snprintf(global_profile_fec_osd, sizeof(global_profile_fec_osd), "%d/%d", new_fec_k, new_fec_n);
    
    // If increasing bitrate, change FEC first; otherwise, bitrate first.
    if (new_bitrate > old_bitrate) {
        // Format fecCommand
        const char *fecKeys[] = { "fecK", "fecN" };
        char strFecK[10], strFecN[10];
        snprintf(strFecK, sizeof(strFecK), "%d", new_fec_k);
        snprintf(strFecN, sizeof(strFecN), "%d", new_fec_n);
        const char *fecValues[] = { strFecK, strFecN };
        format_command(fecCommand, sizeof(fecCommand), fecCommandTemplate, 2, fecKeys, fecValues);
        execute_command(fecCommand);
        old_fec_k = new_fec_k;
        old_fec_n = new_fec_n;
        
        // Format bitrateCommand
        const char *brKeys[] = { "bitrate" };
        char strBitrate[12];
        snprintf(strBitrate, sizeof(strBitrate), "%d", new_bitrate);
        const char *brValues[] = { strBitrate };
        format_command(bitrateCommand, sizeof(bitrateCommand), bitrateCommandTemplate, 1, brKeys, brValues);
        execute_command(bitrateCommand);
        old_bitrate = new_bitrate;
    } else {
        // Format bitrateCommand first
        const char *brKeys[] = { "bitrate" };
        char strBitrate[12];
        snprintf(strBitrate, sizeof(strBitrate), "%d", new_bitrate);
        const char *brValues[] = { strBitrate };
        format_command(bitrateCommand, sizeof(bitrateCommand), bitrateCommandTemplate, 1, brKeys, brValues);
        execute_command(bitrateCommand);
        old_bitrate = new_bitrate;
        
        // Then format fecCommand
        const char *fecKeys[] = { "fecK", "fecN" };
        char strFecK[10], strFecN[10];
        snprintf(strFecK, sizeof(strFecK), "%d", new_fec_k);
        snprintf(strFecN, sizeof(strFecN), "%d", new_fec_n);
        const char *fecValues[] = { strFecK, strFecN };
        format_command(fecCommand, sizeof(fecCommand), fecCommandTemplate, 2, fecKeys, fecValues);
        execute_command(fecCommand);
        old_fec_k = new_fec_k;
        old_fec_n = new_fec_n;
    }
}


void apply_profile(Profile* profile) {
    char powerCommand[MAX_COMMAND_SIZE];
    char fpsCommand[MAX_COMMAND_SIZE];
    char qpDeltaCommand[MAX_COMMAND_SIZE];
    char mcsCommand[MAX_COMMAND_SIZE];
    char gopCommand[MAX_COMMAND_SIZE];
    char roiCommand[MAX_COMMAND_SIZE];
    const char *idrCommand = idrCommandTemplate;  // No formatting needed

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
    int currentBandwidth = profile->bandwidth;
    int currentQpDelta = profile->setQpDelta;
	
    int currentDivideFpsBy = 1;
    int currentFPS = global_fps;
	
    // Determine FPS limit
    if (limitFPS && currentSetBitrate < 4000 && global_fps > 30 && total_pixels > 1300000) {
        currentFPS = 30;
        currentDivideFpsBy = round((double)global_fps / 30);
    } else if (limitFPS && currentSetBitrate < 8000 && total_pixels > 1300000 && global_fps > 60) {
        currentFPS = 60;
        currentDivideFpsBy = round((double)global_fps / 60);
    }
    
    // --- qpDeltaCommand ---
    {
        const char *keys[] = { "qpDelta" };
        char strQpDelta[10];
        snprintf(strQpDelta, sizeof(strQpDelta), "%d", currentQpDelta);
        const char *values[] = { strQpDelta };
        format_command(qpDeltaCommand, sizeof(qpDeltaCommand), qpDeltaCommandTemplate, 1, keys, values);
    }
    // --- fpsCommand ---
    {
        const char *keys[] = { "fps" };
        char strFPS[10];
        snprintf(strFPS, sizeof(strFPS), "%d", currentFPS);
        const char *values[] = { strFPS };
        format_command(fpsCommand, sizeof(fpsCommand), fpsCommandTemplate, 1, keys, values);
    }
    // --- powerCommand ---
    {
        const char *keys[] = { "power" };
        char strPower[10];
        snprintf(strPower, sizeof(strPower), "%d", currentWfbPower * tx_factor);
        const char *values[] = { strPower };
        format_command(powerCommand, sizeof(powerCommand), powerCommandTemplate, 1, keys, values);
    }
    // --- gopCommand ---
    {
        const char *keys[] = { "gop" };
        char strGop[10];
        snprintf(strGop, sizeof(strGop), "%.1f", currentSetGop);
        const char *values[] = { strGop };
        format_command(gopCommand, sizeof(gopCommand), gopCommandTemplate, 1, keys, values);
    }
    // --- mcsCommand ---
    {
        const char *keys[] = { "bandwidth", "gi", "stbc", "ldpc", "mcs" };
        char strBandwidth[10], strGI[10], strStbc[10], strLdpc[10], strMcs[10];
        snprintf(strBandwidth, sizeof(strBandwidth), "%d", currentBandwidth);
        snprintf(strGI, sizeof(strGI), "%s", currentSetGI);
        snprintf(strStbc, sizeof(strStbc), "%d", stbc);
        snprintf(strLdpc, sizeof(strLdpc), "%d", ldpc_tx);
        snprintf(strMcs, sizeof(strMcs), "%d", currentSetMCS);
        const char *values[] = { strBandwidth, strGI, strStbc, strLdpc, strMcs };
        format_command(mcsCommand, sizeof(mcsCommand), mcsCommandTemplate, 5, keys, values);
    }
    // --- roiCommand ---
    {
        const char *keys[] = { "roiQp" };
        const char *values[] = { currentROIqp };
        format_command(roiCommand, sizeof(roiCommand), roiCommandTemplate, 1, keys, values);
    }
    
    // --- Execution Logic ---
    if (currentProfile > previousProfile) {
        if (currentQpDelta != prevQpDelta) {
            execute_command(qpDeltaCommand);
            prevQpDelta = currentQpDelta;
        }
        if (currentFPS != prevFPS) {
            execute_command(fpsCommand);
            prevFPS = currentFPS;
        }
        if (allow_set_power && currentWfbPower != prevWfbPower) {
            execute_command(powerCommand);
            prevWfbPower = currentWfbPower;
        }
        if (currentSetGop != prevSetGop) {
            execute_command(gopCommand);
            prevSetGop = currentSetGop;
        }
        if (strcmp(currentSetGI, prevSetGI) != 0 ||
            currentSetMCS != prevSetMCS ||
            currentBandwidth != prevBandwidth) {
            execute_command(mcsCommand);
            prevBandwidth = currentBandwidth;
            strcpy(prevSetGI, currentSetGI);
            prevSetMCS = currentSetMCS;
        }
        		
        if (currentSetFecK != prevSetFecK || currentSetFecN != prevSetFecN || currentSetBitrate != prevSetBitrate) {
           
		    manage_fec_and_bitrate(currentSetFecK, currentSetFecN, currentSetBitrate);

            prevSetBitrate = currentSetBitrate;
			
            prevSetFecK = currentSetFecK;
            prevSetFecN = currentSetFecN;
        }
		
        if (roi_focus_mode && strcmp(currentROIqp, prevROIqp) != 0) {
            execute_command(roiCommand);
            strcpy(prevROIqp, currentROIqp);
        }
        if (idr_every_change) {
            execute_command(idrCommand);
        }
    } else {
        if (currentQpDelta != prevQpDelta) {
            execute_command(qpDeltaCommand);
            prevQpDelta = currentQpDelta;
        }
        if (currentFPS != prevFPS) {
            execute_command(fpsCommand);
            prevFPS = currentFPS;
        }
        
		if (currentSetFecK != prevSetFecK || currentSetFecN != prevSetFecN || currentSetBitrate != prevSetBitrate) {
           
		    manage_fec_and_bitrate(currentSetFecK, currentSetFecN, currentSetBitrate);

            prevSetBitrate = currentSetBitrate;
			
            prevSetFecK = currentSetFecK;
            prevSetFecN = currentSetFecN;
        }
		
        if (currentSetGop != prevSetGop) {
            execute_command(gopCommand);
            prevSetGop = currentSetGop;
        }
        if (strcmp(currentSetGI, prevSetGI) != 0 ||
            currentSetMCS != prevSetMCS ||
            currentBandwidth != prevBandwidth) {
            execute_command(mcsCommand);
            prevBandwidth = currentBandwidth;
            strcpy(prevSetGI, currentSetGI);
            prevSetMCS = currentSetMCS;
        }
        if (allow_set_power && currentWfbPower != prevWfbPower) {
            execute_command(powerCommand);
            prevWfbPower = currentWfbPower;
        }
        if (roi_focus_mode && strcmp(currentROIqp, prevROIqp) != 0) {
            execute_command(roiCommand);
            strcpy(prevROIqp, currentROIqp);
        }
        if (idr_every_change) {
            execute_command(idrCommand);
        }
    }

    // Update OSD with actual values from wfb_tx_cmd output.
    int k, n, stbc_val, ldpc_val, short_gi, actual_bandwidth, mcs_index, vht_mode, vht_nss;
    read_wfb_tx_cmd_output(&k, &n, &stbc_val, &ldpc_val, &short_gi, &actual_bandwidth, &mcs_index, &vht_mode, &vht_nss);
    const char *gi_string = short_gi ? "short" : "long";
    int pwr = allow_set_power ? profile->wfbPower : 0;
    sprintf(global_profile_osd, "%lds %d %d%s%d Pw%d g%.1f", 
            timeElapsed, 
            profile->setBitrate, 
            actual_bandwidth,
            gi_string,
            mcs_index,
            pwr,
            profile->setGop);
	
    snprintf(global_profile_fec_osd, sizeof(global_profile_fec_osd), "%d/%d", k, n);
}



void *periodic_update_osd(void *arg) {
    osd_udp_config_t *osd_config = (osd_udp_config_t *)arg;

    struct sockaddr_in udp_out_addr;
    if (osd_config->udp_out_sock != -1) {
        // Initialize the target address for UDP
        memset(&udp_out_addr, 0, sizeof(udp_out_addr));
        udp_out_addr.sin_family = AF_INET;
        udp_out_addr.sin_port = htons(osd_config->udp_out_port);
        if (inet_pton(AF_INET, osd_config->udp_out_ip, &udp_out_addr.sin_addr) <= 0) {
            perror("Invalid IP address for OSD UDP output");
            pthread_exit(NULL);
        }
    }

    while (true) {
        sleep(1);
    
    // Generate extra stats string
    snprintf(global_extra_stats_osd, sizeof(global_extra_stats_osd),
             "pnlt%d xtx%ld(%d) gs_idr%d",
             noise_pnlty,
             global_total_tx_dropped,
             total_keyframe_requests_xtx,
             total_keyframe_requests);
    
    // Append the persistent VTX antenna warning if detected
    if (weak_antenna_detected) {
        strncat(global_extra_stats_osd,
                "\nPersistent VTX antenna mismatch >= 20dB detected! Check antennas...",
                sizeof(global_extra_stats_osd) - strlen(global_extra_stats_osd) - 1);
		        printf("Weak drone antenna detected!\n");
    }
		
		// Check if profile is low and set red, or yellow. Otherwise set green
		set_osd_colour = (previousProfile < 1) ? 2 : (previousProfile < 2) ? 5 : 3;
	
		
		// Insert osd font colour and size in regular string
		char local_regular_osd[64];
		snprintf(local_regular_osd, sizeof(local_regular_osd), global_regular_osd, set_osd_colour, set_osd_font_size);

		char full_osd_string[600];
		
		// Combine all osd strings, decide, based on osd_level what to display
		if (osd_level >= 5) {		 // everything, over multiple lines
			snprintf(full_osd_string, sizeof(full_osd_string), "%s %s\n%s\n%s\n%s\n%s",
					global_profile_osd, global_profile_fec_osd, local_regular_osd, global_score_related_osd, global_gs_stats_osd, global_extra_stats_osd);
					
		} else if (osd_level == 4) { // everything on one line
			snprintf(full_osd_string, sizeof(full_osd_string), "%s %s | %s | %s | %s | %s",
					global_profile_osd, global_profile_fec_osd, local_regular_osd, global_score_related_osd, global_gs_stats_osd, global_extra_stats_osd);
					
        } else if (osd_level == 3) { // medium extras
			snprintf(full_osd_string, sizeof(full_osd_string), "%s %s %s\n%s",
					global_profile_osd, global_profile_fec_osd, local_regular_osd, global_gs_stats_osd);	
			
		} else if (osd_level == 2) { // minimal extras
			snprintf(full_osd_string, sizeof(full_osd_string), "%s %s %s",
					global_profile_osd, global_profile_fec_osd, local_regular_osd);	
			
		} else if (osd_level == 1){ // only basic regular string
			snprintf(full_osd_string, sizeof(full_osd_string), "%s",
					local_regular_osd);	
			
		}	
		if (osd_level != 0) { // only if enabled
			// Either update OSD remotely over udp, or update local file
			if (osd_config->udp_out_sock != -1) {
				// Send the OSD string over UDP
				ssize_t sent_bytes = sendto(osd_config->udp_out_sock, full_osd_string, strlen(full_osd_string), 0,
                                        (struct sockaddr *)&udp_out_addr, sizeof(udp_out_addr));
				if (sent_bytes < 0) {
					perror("Error sending OSD string over UDP");
				}
			} else {
				// Write to /tmp/MSPOSD.msg
				FILE *file = fopen("/tmp/MSPOSD.msg", "w");
				if (file == NULL) {
					perror("Error opening /tmp/MSPOSD.msg");
					continue; // Skip this iteration if the file cannot be opened
				}

				if (fwrite(full_osd_string, sizeof(char), strlen(full_osd_string), file) != strlen(full_osd_string)) {
					perror("Error writing to /tmp/MSPOSD.msg");
				}

				fclose(file);
			}
		}
	    // Don't continue updating OSD until initialized
		
		while (!initialized_by_first_message) {
			sleep(1);  // Wait until initialized_by_first_message becomes true
		}
		
    }
    return NULL;
}

bool value_chooses_profile(int input_value) {
    // Get the appropriate profile based on input
    Profile* selectedProfile = get_profile(input_value);
    if (selectedProfile == NULL) {
        printf("No matching profile found for input: %d\n", input_value);
		return false;
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
		return false;
    }

    // Check if a change is needed based on time constraints
    long currentTime = get_monotonic_time();
    long timeElapsed = currentTime - prevTimeStamp;

	// if it's in fallback, go by fallback time
    if (previousProfile == 0) {
		if (timeElapsed <= hold_fallback_mode_s) {
			if (verbose_mode) {
				puts("Holding fallback...");
			}
			return false;
		}
	}
	// or if it's a normal profile, go by normal holddown time
    else if (previousProfile < currentProfile && timeElapsed <= hold_modes_down_s) {
        if (verbose_mode) {
			puts("Too soon to increase link...");
		}
		return false;
    }

    // Apply the selected profile
    apply_profile(selectedProfile);
	// Update previousProfile
    previousProfile = currentProfile;
	prevTimeStamp = currentTime;
	return true;

}

void start_selection(int rssi_score, int snr_score, int recovered) {

    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    // Shortcut for fallback profile 999
    if (rssi_score == 999) {
        if (value_chooses_profile(999)) {
            printf("Applied.\n");
            last_value_sent = 999;
            smoothed_combined_value = 999;
            last_exec_time = current_time;
        } else {
            printf("Not applied.\n");
		}
		return;
    }

	if (selection_busy) {
        if (verbose_mode) {
			puts("Selection process busy...");
		}
		return;
    }
    selection_busy = true;

    // Combine rssi and snr by weight
	float combined_value_float = rssi_score * rssi_weight + snr_score * snr_weight;
	int osd_raw_score = (int)combined_value_float;

	// Determine which exp_smoothing_factor to use (up or down)
    float chosen_smoothing_factor = (combined_value_float >= last_value_sent) ? smoothing_factor : smoothing_factor_down;

	// Apply exponential smoothing
    smoothed_combined_value = (chosen_smoothing_factor * combined_value_float + (1 - chosen_smoothing_factor) * smoothed_combined_value);

	int osd_smoothed_score = (int)smoothed_combined_value;
	// update score_related osd string
	sprintf(global_score_related_osd, "linkQ %d, smthdQ %d", osd_raw_score, osd_smoothed_score);

	// Check if enough time has passed
    long time_diff_ms = (current_time.tv_sec - last_exec_time.tv_sec) * 1000 + (current_time.tv_nsec - last_exec_time.tv_nsec) / 1000000;
    if (time_diff_ms < min_between_changes_ms) {
        printf("Skipping profile load: time_diff_ms=%ldms - too soon (min %dms required)\n", time_diff_ms, min_between_changes_ms);
        selection_busy = false;
        return;
    }
	// Clamp combined value within the defined range
    int combined_value = (int)floor(smoothed_combined_value);
    combined_value = (combined_value < 1000) ? 1000 : (combined_value > 2000) ? 2000 : combined_value;

    // Calculate percentage change from smoothed baseline value
    float percent_change = fabs((float)(combined_value - last_value_sent) / last_value_sent) * 100;

    // Determine which hysteresis threshold to use (up or down)
    float hysteresis_threshold = (combined_value >= last_value_sent) ? hysteresis_percent : hysteresis_percent_down;

    // Check if the change exceeds the chosen hysteresis threshold
    if (percent_change >= hysteresis_threshold) {
        printf("Qualified to request profile: %d is > %.2f%% different (%.2f%%)\n", combined_value, hysteresis_threshold, percent_change);

        // Request profile, check if applied
        if (value_chooses_profile(combined_value)) {
            printf("Profile %d applied.\n", combined_value);
            last_value_sent = combined_value;
            last_exec_time = current_time;
        }
    }
    selection_busy = false;
}


// request_keyframe function to check if a code exists in the array and has not expired
bool code_exists(const char *code, struct timespec *current_time) {
    for (int i = 0; i < num_keyframe_requests; i++) {
        if (strcmp(keyframe_request_codes[i].code, code) == 0) {
            // Check if the request is still valid
            long elapsed_time_ms = (current_time->tv_sec - keyframe_request_codes[i].timestamp.tv_sec) * 1000 +
                                   (current_time->tv_nsec - keyframe_request_codes[i].timestamp.tv_nsec) / 1000000;
            if (elapsed_time_ms < EXPIRY_TIME_MS) {
                return true;  // Code exists and has not expired
            } else {
                // Expired: Remove it by shifting the rest down
                memmove(&keyframe_request_codes[i], &keyframe_request_codes[i + 1],
                        (num_keyframe_requests - i - 1) * sizeof(KeyframeRequest));
                num_keyframe_requests--;
				i--;  // Adjust index to re-check at this position after shift
                return false;  // Code expired
            }
        }
    }
    return false;  // Code not found
}

// Function to add a code to the array
void add_code(const char *code, struct timespec *current_time) {
    if (num_keyframe_requests < MAX_CODES) {
        strncpy(keyframe_request_codes[num_keyframe_requests].code, code, CODE_LENGTH);
        keyframe_request_codes[num_keyframe_requests].timestamp = *current_time;
        num_keyframe_requests++;
    } else {
        printf("Max keyframe request codes reached. Consider increasing MAX_CODES.\n");
    }
}

void cleanup_expired_codes(struct timespec *current_time) {
    for (int i = 0; i < num_keyframe_requests; ) {
        // Calculate elapsed time in milliseconds
        long elapsed_time_ms = (current_time->tv_sec - keyframe_request_codes[i].timestamp.tv_sec) * 1000 +
                               (current_time->tv_nsec - keyframe_request_codes[i].timestamp.tv_nsec) / 1000000;

        // Remove the expired entry if elapsed time exceeds expiry threshold
        if (elapsed_time_ms >= EXPIRY_TIME_MS) {
            memmove(&keyframe_request_codes[i], &keyframe_request_codes[i + 1],
                    (num_keyframe_requests - i - 1) * sizeof(KeyframeRequest));
            num_keyframe_requests--;  // Decrease the count of requests
        } else {
            i++;  // Only move to the next entry if no removal
        }
    }
}

// Main function to handle special commands
void special_command_message(const char *msg) {
    const char *cleaned_msg = msg + 8;  // Skip "special:"
    const char *idrCommand = idrCommandTemplate;

    char *separator = strchr(cleaned_msg, ':');
    char code[CODE_LENGTH] = {0};  // Buffer for unique request code

    if (separator) {
        *separator = '\0';  // Split at the first ':'
        strncpy(code, separator + 1, CODE_LENGTH - 1);  // Copy unique code if present
    }

    // Check for keyframe request first
    if (allow_request_keyframe && prevSetGop > 0.5 && strcmp(cleaned_msg, "request_keyframe") == 0 && code[0] != '\0') {
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        
        // Clean up expired codes before proceeding
        cleanup_expired_codes(&current_time);

        // Check if the keyframe request interval has elapsed
        long elapsed_ms = (current_time.tv_sec - last_keyframe_request_time.tv_sec) * 1000 +
                          (current_time.tv_nsec - last_keyframe_request_time.tv_nsec) / 1000000;
        
        if (elapsed_ms >= request_keyframe_interval_ms) {
            if (!code_exists(code, &current_time)) {
                add_code(code, &current_time);  // Store new code and timestamp

                // Request new keyframe
                char quotedCommand[BUFFER_SIZE];
                snprintf(quotedCommand, sizeof(quotedCommand), "\"%s\"", idrCommand);
                if (verbose_mode) {
                    printf("Special: Requesting Keyframe for code: %s\n", code);
                }
				if (system(quotedCommand) != 0) { printf("Command failed: %s\n", quotedCommand); }
                last_keyframe_request_time = current_time;
				total_keyframe_requests++;
            } else {
                if (verbose_mode) {
					printf("Already requested keyframe for code: %s\n", code);
				}
			}
        } else {
                if (verbose_mode) {
					printf("Keyframe request ignored. Interval not met for code: %s\n", code);
				}
        }

    } else if (strcmp(cleaned_msg, "pause_adaptive") == 0) {
        pthread_mutex_lock(&pause_mutex);
        paused = true;
        pthread_mutex_unlock(&pause_mutex);
        printf("Paused adaptive mode\n");

    } else if (strcmp(cleaned_msg, "resume_adaptive") == 0) {
        pthread_mutex_lock(&pause_mutex);
        paused = false;
        pthread_mutex_unlock(&pause_mutex);
        printf("Resumed adaptive mode\n");

    } else {
        printf("Unknown or disabled special command: %s\n", cleaned_msg);
    }
}


//function to get latest tx dropped
long get_wlan0_tx_dropped() {
    FILE *fp;
    char line[256];
    long tx_dropped = 0;

    // Open the /proc/net/dev file
    fp = fopen("/proc/net/dev", "r");
    if (fp == NULL) {
        perror("Failed to open /proc/net/dev");
        return -1;
    }

    // Skip the first two lines (headers)
    if (fgets(line, sizeof(line), fp) == NULL || fgets(line, sizeof(line), fp) == NULL) { printf("fgets failed\n"); }

    // Read each line to find the wlan0 interface
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, "wlan0:") != NULL) {
            // Locate the stats after the "wlan0:" label
            char *stats_str = strchr(line, ':');
            if (stats_str) {
                stats_str++;  // Move past the colon

                // Tokenize to skip to the 12th field
                char *token;
                int field_count = 0;
                token = strtok(stats_str, " ");

                while (token != NULL) {
                    field_count++;
                    if (field_count == 12) {
                        tx_dropped = strtol(token, NULL, 10);
                        break;
                    }
                    token = strtok(NULL, " ");
                }
            }
            break;
        }
    }

    // Close the file
    fclose(fp);
	
    //calculate difference
	long latest_tx_dropped = tx_dropped - global_total_tx_dropped;
	//update global total
	global_total_tx_dropped = tx_dropped;
	return latest_tx_dropped;

}

void *periodic_tx_dropped(void *arg) {
    
    const char *idrCommand = idrCommandTemplate;

	// Wait until initialized_by_first_message
	while (!initialized_by_first_message) {
		sleep(1);
	}
	
    while (1) {
        long latest_tx_dropped = get_wlan0_tx_dropped();

		struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        long elapsed_ms = (current_time.tv_sec - last_keyframe_request_time.tv_sec) * 1000 +
                          (current_time.tv_nsec - last_keyframe_request_time.tv_nsec) / 1000000;

        if (latest_tx_dropped > 0 && elapsed_ms >= request_keyframe_interval_ms && allow_rq_kf_by_tx_d && prevSetGop > 0.5) {
         
			// Request new keyframe
			char quotedCommand[BUFFER_SIZE];
			snprintf(quotedCommand, sizeof(quotedCommand), "\"%s\"", idrCommand);
					
			if (system(quotedCommand) != 0) { printf("Command failed: %s\n", quotedCommand); }
			last_keyframe_request_time = current_time;
			total_keyframe_requests_xtx++;
					
			if (verbose_mode) {
				printf("Requesting keyframe for locally dropped tx packet\n");
			}
        }

        usleep(check_xtx_period_ms * 1000);
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
        if (initialized_by_first_message && local_count == 0 && !paused) {
            printf("No messages received in %dms, sending 999\n", fallback_ms);
            start_selection(999, 1000, 0);
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
    
	static struct timeval last_fec_call_time = {0};
    static int first_time = 1;

    // Declare default local variables
    struct timeval tv;
    int transmitted_time = 0;
    int link_value_rssi = 999;
    int link_value_snr = 999;
    int recovered = 0;
    int recovered_otime = 0;
    int rssi1 = -105;
    int snr1 = 0;
    char idr_code[16] = "";

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
                recovered_otime = atoi(token);
                break;
            case 5:
                rssi1 = atoi(token);
                break;
            case 6:
                snr1 = atoi(token);
                break;
            case 7:
                num_antennas = atoi(token);
                break;
            case 8:
                noise_pnlty = atoi(token);
                break;
            case 9:
                fec_change = atoi(token);
                break;
            case 10:
                strncpy(idr_code, token, sizeof(idr_code) - 1);
                idr_code[sizeof(idr_code) - 1] = '\0';
                break;
            default:
                // Ignore extra tokens
                break;
        }
        token = strtok(NULL, ":");
        index++;
    }

    // Free the duplicated string
    free(msgCopy);

    // Request a keyframe if an idr_code is provided
    if (idr_code[0] != '\0') {
        char keyframe_request[64];
        snprintf(keyframe_request, sizeof(keyframe_request), "special:request_keyframe:%s", idr_code);
        special_command_message(keyframe_request);
    }

    // Get the current time
    struct timeval current_time;
    gettimeofday(&current_time, NULL);

    // Initialize last_fec_call_time on the first call
    if (first_time) {
        last_fec_call_time = current_time;
        first_time = 0;
    }

    // Calculate the time difference in milliseconds
    long elapsed_ms = (current_time.tv_sec - last_fec_call_time.tv_sec) * 1000 +
                      (current_time.tv_usec - last_fec_call_time.tv_usec) / 1000;

    // Only call manage_fec_and_bitrate if a fec_change has occurred and
    // at least 1 second has elapsed since the last call
    if (allow_dynamic_fec && fec_change != prev_fec_change && elapsed_ms >= 1000) {
        manage_fec_and_bitrate(prevSetFecK, prevSetFecN, prevSetBitrate);
        last_fec_call_time = current_time;
        prev_fec_change = fec_change;
    }

    // Create OSD string with ground station stats information
	if (num_antennas_drone > 0) {
		sprintf(global_gs_stats_osd, "rssi%d snr%d ants:vrx%d,vtx%d", rssi1, snr1, num_antennas, num_antennas_drone);
	} else {
		sprintf(global_gs_stats_osd, "rssi%d snr%d ants:vrx%d", rssi1, snr1, num_antennas);
	}

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

    // Start selection if not paused
    pthread_mutex_lock(&pause_mutex);
    if (!paused) {
        start_selection(link_value_rssi, link_value_snr, recovered);
    } else {
        printf("Adaptive mode paused, waiting for resume command...\n");
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
    load_config(CONFIG_FILE);
    load_profiles(PROFILE_FILE);

    int sockfd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];
    int port = DEFAULT_PORT;
    char ip[INET_ADDRSTRLEN] = DEFAULT_IP; // Default IP

    // Initialize osd_udp_config_t struct
    osd_udp_config_t osd_config = { .udp_out_sock = -1 };

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

        } else if (strcmp(argv[i], "--osd2udp") == 0 && i + 1 < argc) {
            char *ip_port = argv[++i];
            char *colon_pos = strchr(ip_port, ':');
            if (colon_pos) {
                *colon_pos = '\0'; // Split IP and port
                strncpy(osd_config.udp_out_ip, ip_port, INET_ADDRSTRLEN);
                osd_config.udp_out_port = atoi(colon_pos + 1);
            } else {
                fprintf(stderr, "Invalid format for --osd2udp. Expected <ip:port>\n");
                return 1;
            }

            // Create the outgoing UDP socket
            if ((osd_config.udp_out_sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("Error creating outgoing UDP socket");
                return 1;
            }

            printf("OSD UDP output enabled to %s:%d\n", osd_config.udp_out_ip, osd_config.udp_out_port);
        } else {
            print_usage();
            return 1;
        }
    }

    // Create UDP socket for incoming messages
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed. TX connected? Make sure video and tunnel are working");
		error_to_osd("Adaptive-Link:  Check wfb tunnel functionality");
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
		error_to_osd("Adaptive-Link:  Check wfb tunnel functionality");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Listening on UDP port %d, IP: %s...\n", port, ip);
	
	// Find out wifi card
	determine_tx_power_equation();
    printf("TX Power Factor: %d\n", tx_factor);

	// Get any required values from wfb.yaml (eg ldpc_tx and stbc)
	if (get_card_info_from_yaml) {
		load_from_vtx_info_yaml();
	}
	// Print S and L
	printf("ldpc_tx: %d\nstbc: %d\n", ldpc_tx, stbc);
	
	// Get resolution
	if (get_resolution() != 0) {
					printf("Failed to get resolution. Assuming 1920x1080\n");
					x_res = 1920;
					y_res = 1080;
	}
	
	total_pixels = x_res * y_res;

	set_osd_font_size = (x_res < 1280) ? ((int)(20 * multiply_font_size_by)) :
                    (x_res < 1700) ? ((int)(25 * multiply_font_size_by)) :
                    (x_res < 2000) ? ((int)(35 * multiply_font_size_by)) :
                    (x_res < 2560) ? ((int)(45 * multiply_font_size_by)) :
                                    ((int)(50 * multiply_font_size_by));

	
	//Get fps value from majestic
	int fps = get_video_fps();
    if (fps >= 0) {
        printf("Video FPS: %d\n", fps);
		global_fps = fps;
		if (fps == 0) {
			limitFPS = 0;
		}
    } else {
        printf("Failed to retrieve video FPS from majestic.\n");
		limitFPS = 0;	
    }
    // Check if roi_focus_mode is enabled and call the setup_roi function
    if (roi_focus_mode) {
        if (setup_roi() != 0) {
            printf("Failed to set up focus mode regions based on majestic resolution\n");
        } else {
            printf("Focus mode regions set in majestic.yaml\n");
        }
    }

	// Start drone antenna monitoring thread
	pthread_t rssi_thread;
	if (pthread_create(&rssi_thread, NULL, parse_rssi_thread, NULL)) {
		fprintf(stderr, "Error creating drone RSSI monitoring thread\n");
	}

    // Start the counting thread
    pthread_t count_thread;
    pthread_create(&count_thread, NULL, count_messages, NULL);

    // Start the periodic OSD update thread, passing osd_config
    pthread_t osd_thread;
    pthread_create(&osd_thread, NULL, periodic_update_osd, &osd_config);

    // Start the periodic TX dropped thread
    pthread_t tx_dropped_thread;
    pthread_create(&tx_dropped_thread, NULL, periodic_tx_dropped, NULL);

    // Main loop for processing incoming messages
    while (1) {
        // Receive a message
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                         (struct sockaddr *)&client_addr, &client_addr_len);
        if (n < 0) {
            perror("recvfrom failed");
            break;
        }

		initialized_by_first_message = true;


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
        // See if it's a special command, otherwise process it
        if (strncmp(message, "special:", 8) == 0) {
            special_command_message(message);
        } else {
            process_message(message);
        }
    }

    // Close the socket
    close(sockfd);

    // Close outgoing OSD socket if it was opened
    if (osd_config.udp_out_sock != -1) {
        close(osd_config.udp_out_sock);
    }

    return 0;
}


