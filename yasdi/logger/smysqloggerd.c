/*  Copyright (C) 2011-2012 Vincent Deconinck (known on google mail as user vdeconinck)

    This file is part of the SMySqLogger project.
	
    SMySqLogger is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
	
    SMySqLogger is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with SMySqLogger.  If not, see <http://www.gnu.org/licenses/>.

 This is the daemon version of the logger

 To compile:	cc -o mysqloggerd mysqloggerd.c
 To run:		./mysqloggerd
 To test daemon:	ps -ef|grep mysqloggerd (or ps -aux on BSD systems)
 To test log:	tail -f /tmp/mysqloggerd.log
 To test signal:	kill -HUP `cat /tmp/mysqloggerd.lock`
 To terminate:	kill `cat /tmp/mysqloggerd.lock`
 
 */

// TODO : standardize logging (only if -verbose ? only if not daemon ? use stderr or stdout ?)
 
/*************************************************************************
*   I N C L U D E
*************************************************************************/
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "os.h"
#include "smadef.h"
#include "chandef.h"
#include "libyasdi.h"
#include "libyasdimaster.h"
#include "smadata_layer.h"
#include "tools.h"
#include "db.h"
#include "consts.h"
#include "util.h"

#ifdef __cplusplus
}
#endif

/**************************************************************************
*   G L O B A L
**************************************************************************/

#define MAXDRIVERS 10   // for simplicity, we reserve 10 YASDI Bus drivers

#define RUNNING_DIR	"/tmp"
#define LOCK_FILE	"/tmp/smysqloggerd.pid"
// TODO make log destination a config param (detect stdout & stderr as constants, everything else is a path of a file to open)
#define LOG_FILE	"/tmp/smysqloggerd.log"

#define YASDI_INI   "yasdi.ini"

/**************************************************************************
*   S T A T I C
**************************************************************************/

// This is the list of the inverter properties that will be fetched and stored in DB (aka "Channels").
// The values below are based on a SMC4600 inverter.
// You can remove some if you don't need them to keep the DB lighter, but please make sure to edit the 3 arrays consistently.
// Channel names used by the inverter (!case sensitive !)
char channelNames[][MAX_CHANNEL_NAME_LEN] = 
{"Upv-Ist", "Upv-Soll", "Iac-Ist", "Uac", "Fac", "Pac", "Zac", "Riso", "Ipv", "E-Total", "h-Total", "h-On", "Netz-Ein", "Seriennummer", "Status", "Balancer", "Fehler"};

// Names of the database columns to store the values in
char dbColumns[][MAX_COLUMN_NAME_LEN] = 
{"upv_ist_volt", "upv_soll_volt", "iac_ist_amp", "uac_volt", "fac_hz", "pac_watt", "zac_ohm", "riso_kohm", "ipv_amp", "e_total_kwh", "h_total_hour", "h_on_hour", "netz_ein", "seriennummer", "status", "balancer", "fehler"};

// Types of the database columns to store the values in
char dbTypes[][MAX_COLUMN_TYPE_LEN] = 
{"SMALLINT UNSIGNED", "SMALLINT UNSIGNED", "FLOAT UNSIGNED", "SMALLINT UNSIGNED", "FLOAT UNSIGNED", "SMALLINT UNSIGNED", "FLOAT UNSIGNED", "FLOAT UNSIGNED", "FLOAT UNSIGNED", "DOUBLE UNSIGNED", "DOUBLE UNSIGNED", "DOUBLE UNSIGNED", "MEDIUMINT UNSIGNED", "BIGINT UNSIGNED", "VARCHAR(10)", "VARCHAR(10)", "VARCHAR(10)"};


// Channel names used by Sunny Island
char SIchannelNames[][MAX_CHANNEL_NAME_LEN] = 
{"Upv-Ist", "Upv-Soll", "Iac-Ist", "Uac", "Fac", "Pac", "Zac", "Riso", "Ipv", "E-Total", "h-Total", "h-On", "Netz-Ein", "Seriennummer", "Status", "Balancer", "Fehler"};

// Names of the database columns to store the values in
char SIdbColumns[][MAX_COLUMN_NAME_LEN] = 
{"upv_ist_volt", "upv_soll_volt", "iac_ist_amp", "uac_volt", "fac_hz", "pac_watt", "zac_ohm", "riso_kohm", "ipv_amp", "e_total_kwh", "h_total_hour", "h_on_hour", "netz_ein", "seriennummer", "status", "balancer", "fehler"};

// Types of the database columns to store the values in
char SIdbTypes[][MAX_COLUMN_TYPE_LEN] = 
{"SMALLINT UNSIGNED", "SMALLINT UNSIGNED", "FLOAT UNSIGNED", "SMALLINT UNSIGNED", "FLOAT UNSIGNED", "SMALLINT UNSIGNED", "FLOAT UNSIGNED", "FLOAT UNSIGNED", "FLOAT UNSIGNED", "DOUBLE UNSIGNED", "DOUBLE UNSIGNED", "DOUBLE UNSIGNED", "MEDIUMINT UNSIGNED", "BIGINT UNSIGNED", "VARCHAR(10)", "VARCHAR(10)", "VARCHAR(10)"};


/* maximum age of the channel values, in seconds...*/
DWORD maxChannelValueAge; 

/* interval between loops, in seconds */	
int requestedInterval; 

/* indicate that loop should go on */	
BOOL loop = true;


/**************************************************************************
signalHandler
**************************************************************************/
void signalHandler(int sig) {
	switch(sig) {
        case SIGHUP:
            printLog(LEVEL_IMPORTANT, "Received SIGHUP signal. Daemon is alive.\n");
            break;
        case SIGUSR1:
            printLog(LEVEL_IMPORTANT, "Received SIGUSR1 signal. Daemon is alive.\n");
            break;
        case SIGTERM:
            lightLog(LEVEL_IMPORTANT, "\n");
            printLog(LEVEL_IMPORTANT, "Caught terminate signal. Requesting stop of polling loop...\n");
            loop = false;
            break;
	}
}

/**************************************************************************
daemonize
**************************************************************************/
void daemonize() {
    int i, lfp;
    char str[10];
	if(getppid() == 1) return; /* already a daemon */
	
	if (access(LOCK_FILE, F_OK) != -1 ) {
		// PID LOCK FILE EXISTS
        printf("Cannot start. Another instance has created the lock file '%s'\n", LOCK_FILE);
		exit(-1);
	}
	
    i = fork();
    if (i < 0) exit(1); /* fork error */
    if (i > 0) exit(0); /* parent exits */
	/* child (daemon) continues */
	setsid(); /* obtain a new process group */
	// TODO : clean way is to close all descriptors, and reopen dummy ones, but this seems to fail...
	//for (i = getdtablesize(); i>=0; --i) close(i); /* close all descriptors */
	//i = open("/dev/null", O_RDWR); dup(i); dup(i); /* handle standard I/O */
	umask(027); /* set newly created file permissions */
	chdir(RUNNING_DIR); /* change running directory */
	lfp = open(LOCK_FILE, O_RDWR|O_CREAT, 0640);
	if (lfp < 0) exit(1); /* can not open */
	if (lockf(lfp, F_TLOCK, 0) < 0) exit(0); /* can not lock */
	/* first instance continues */
	sprintf(str, "%d\n", getpid());
	write(lfp, str, strlen(str)); /* record pid to lockfile */
	signal(SIGCHLD, SIG_IGN); /* ignore child */
	signal(SIGTSTP, SIG_IGN); /* ignore tty signals */
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGHUP, signalHandler); /* catch hangup signal */
	signal(SIGUSR1, signalHandler); /* catch hangup signal */
	signal(SIGTERM, signalHandler); /* catch kill signal */
}


/**************************************************************************
checkArrayConsistency
**************************************************************************/
int checkArrayConsistency() {
	int channelCount = sizeof(channelNames)/MAX_CHANNEL_NAME_LEN;
	int columnCount = sizeof(dbColumns)/MAX_COLUMN_NAME_LEN;
	int typeCount = sizeof(dbTypes)/MAX_COLUMN_TYPE_LEN;
	int i;

	printLog(LEVEL_DETAIL, "List of requested channels:\n");
    lightLog(LEVEL_DETAIL, "        channel_name -> db_column (db_type)\n");
    lightLog(LEVEL_DETAIL, "------------------------------------------------------------------------------------\n");
	for (i = 0; i < max(channelCount, max(columnCount, typeCount)); i++) {
		lightLog(LEVEL_DETAIL, "%20s -> %s (%s)\n", (i < channelCount ? channelNames[i]:"?"), (i < columnCount ? dbColumns[i]:"?"), (i < typeCount ? dbTypes[i]:"?"));
	}

	// Check size consistency
	if (channelCount != columnCount) { 
		printLog(LEVEL_FATAL, "Error : inconsistent number of channel_names (%d) and db_columns (%d)!\n", channelCount, columnCount);
		exit(-20);
	}
	if (columnCount != typeCount) { 
		printLog(LEVEL_FATAL, "Error : inconsistent number of db_columns (%d) and db_types (%d)!\n", columnCount, typeCount);
		exit(-21);
	}
	
	return channelCount;
}

/**************************************************************************
SIcheckArrayConsistency
**************************************************************************/
int SIcheckArrayConsistency() {
	int SIchannelCount = sizeof(SIchannelNames)/MAX_CHANNEL_NAME_LEN;
	int SIcolumnCount = sizeof(SIdbColumns)/MAX_COLUMN_NAME_LEN;
	int SItypeCount = sizeof(SIdbTypes)/MAX_COLUMN_TYPE_LEN;
	int i;

	printLog(LEVEL_DETAIL, "List of requested channels:\n");
    lightLog(LEVEL_DETAIL, "        SIchannel_name -> db_column (db_type)\n");
    lightLog(LEVEL_DETAIL, "------------------------------------------------------------------------------------\n");
	for (i = 0; i < max(SIchannelCount, max(SIcolumnCount, SItypeCount)); i++) {
		lightLog(LEVEL_DETAIL, "%20s -> %s (%s)\n", (i < SIchannelCount ? SIchannelNames[i]:"?"), (i < SIcolumnCount ? SIdbColumns[i]:"?"), (i < SItypeCount ? SIdbTypes[i]:"?"));
	}

	// Check size consistency
	if (SIchannelCount != SIcolumnCount) { 
		printLog(LEVEL_FATAL, "Error : inconsistent number of SIchannel_names (%d) and SIdb_columns (%d)!\n", SIchannelCount, SIcolumnCount);
		exit(-20);
	}
	if (SIcolumnCount != SItypeCount) { 
		printLog(LEVEL_FATAL, "Error : inconsistent number of SIdb_columns (%d) and SIdb_types (%d)!\n", SIcolumnCount, SItypeCount);
		exit(-21);
	}
	
	return SIchannelCount;
}


/**************************************************************************
logValues
**************************************************************************/
BOOL logValues(int deviceCount, DWORD deviceHandles[], int channelCount) {
	int res, i;

	char channelValues[channelCount][MAX_CHANNEL_VALUE_LEN];
	char chanName[MAX_CHANNEL_NAME_LEN];
	DWORD channelHandles[deviceCount][channelCount]; 
	double value;
	char textValue[MAX_CHANNEL_VALUE_LEN];
	time_t loopStart, dayStart, now;
	double elapsedSecs;
	int deviceNr, channelNr, delay;

	/* init channel handles */
	for (deviceNr = 0; deviceNr < deviceCount; deviceNr++) {
		for (channelNr = 0; channelNr < channelCount; channelNr++) {
			channelHandles[deviceNr][channelNr] = FindChannelName(deviceHandles[deviceNr], channelNames[channelNr]);
			if (channelHandles[deviceNr][channelNr] <= 0) {
				// Throw in some help for debugging purpose...
				DWORD chanHandle[300]; 
				char deviceName[50];
				char cUnit[17];
				int realChannelNr;

				printLog(LEVEL_FATAL, "Error : cannot determine handle of channel name '%s' for device %d\n", channelNames[channelNr], deviceHandles[deviceNr]);
				int realChannelCount = GetChannelHandlesEx(deviceHandles[deviceNr], chanHandle, 300, SPOTCHANNELS);
				GetDeviceName(deviceHandles[deviceNr], deviceName, sizeof(deviceName)-1);
				lightLog(LEVEL_ERROR, "Here are the %d available channels for device '%s':\n", realChannelCount, deviceName);
				lightLog(LEVEL_ERROR, "---------------------------------------\n");
				lightLog(LEVEL_ERROR, "|   Channel name   |   Channel Unit   |\n");
				lightLog(LEVEL_ERROR, "---------------------------------------\n");
				for(realChannelNr = 0; realChannelNr < realChannelCount; realChannelNr++)
				{
					GetChannelName(chanHandle[realChannelNr], chanName, sizeof(chanName)-1);
					cUnit[0]=0;
					GetChannelUnit(chanHandle[realChannelNr], cUnit, sizeof(cUnit)-1);
					lightLog(LEVEL_ERROR, "| %16s | %-16s |\n", chanName, cUnit);
				}
				lightLog(LEVEL_ERROR, "---------------------------------------\n");

				exit(-50);
			}
			GetChannelName(channelHandles[deviceNr][channelNr], chanName, sizeof(chanName)-1);
			printLog(LEVEL_DETAIL, "Channel '%s' has handle %d on device %d\n", channelNames[channelNr], channelHandles[deviceNr][channelNr], deviceHandles[deviceNr]);
		}
	}

	BOOL readOK = true;
	
	// Determine first schedule
	now = time(NULL);
	struct tm *cal = localtime(&now);
	cal->tm_hour = 0;
	cal->tm_min = 0;
	cal->tm_sec = 0;
	dayStart = mktime(cal);
	elapsedSecs = difftime(now, dayStart);

	delay = requestedInterval - ((int)(elapsedSecs) % requestedInterval); 
	printLog(LEVEL_INFO, "Waiting for %d seconds before first trace...\n", delay);
	// Sleep by 1-second interval to allow exit if requested
	for(i=0; (i<delay) && loop; i++) {
		sleep(1);
	}

	printLog(LEVEL_INFO, "Starting loop. Each dot below represents a trace successfully inserted in DB:\n", delay);
	while (loop) {
		loopStart = time(NULL);
		
		// Iterate on devices
		for (deviceNr = 0; deviceNr < deviceCount; deviceNr++) {

            // Iterate on channels for this device
			for (channelNr=0; channelNr<channelCount; channelNr++) {
				res = GetChannelValue(channelHandles[deviceNr][channelNr], deviceHandles[deviceNr], &value, textValue, sizeof(textValue)-1, maxChannelValueAge);
				if (res == 0) {
					if (strlen(textValue) == 0) {
						sprintf(textValue, "%.6f", value);
					}
				}
				else {                    
                    // means connection with at least one device was lost. 
                    if (res != -3) {
                        printLog(LEVEL_WARNING, "Error reading channel value....error code=%d\n", res);
                    }
                    readOK = false;
                    loop = false;
					break;
				}
				strcpy(channelValues[channelNr], textValue);
			}
			
			if (readOK) {
				// Insert row
				insertValues(channelCount, dbColumns, channelValues);
				lightLog(LEVEL_INFO, ".");
			}
		}

		if (loop) {
            // determine processing duration to schedule next loop turn
            now = time(NULL);
            elapsedSecs = difftime(now, loopStart);

            if (elapsedSecs < requestedInterval) {
                // Compute theoretic start of next loop
                struct tm *cal = localtime(&loopStart);
                cal->tm_sec += requestedInterval;
                loopStart = mktime(cal);
                delay = requestedInterval - elapsedSecs;

                // Must sleep now.
				// Sleep by 1-second interval to allow exit if requested
				for(i=0; (i<delay) && loop; i++) {
					sleep(1);
				}
            }
            else {
                printLog(LEVEL_WARNING, "Warning : Processing took %f seconds. Can't keep up with the %d seconds requested interval. Please choose a higher value.\n", elapsedSecs, requestedInterval);
                loopStart = time(NULL);
            }
        }
	}
    return readOK;
}

/**************************************************************************
SIlogValues
**************************************************************************/
BOOL SIlogValues(int SIdeviceCount, DWORD SIdeviceHandles[], int SIchannelCount) {
	int res, i;

	char SIchannelValues[SIchannelCount][MAX_CHANNEL_VALUE_LEN];
	char SIchanName[MAX_CHANNEL_NAME_LEN];
	DWORD SIchannelHandles[SIdeviceCount][SIchannelCount]; 
	double value;
	char textValue[MAX_CHANNEL_VALUE_LEN];
	time_t loopStart, dayStart, now;
	double elapsedSecs;
	int SIdeviceNr, SIchannelNr, delay;

	/* init channel handles */
	for (SIdeviceNr = 0; SIdeviceNr < SIdeviceCount; SIdeviceNr++) {
		for (SIchannelNr = 0; SIchannelNr < SIchannelCount; SIchannelNr++) {
			SIchannelHandles[SIdeviceNr][SIchannelNr] = FindChannelName(SIdeviceHandles[SIdeviceNr], SIchannelNames[SIchannelNr]);
			if (SIchannelHandles[SIdeviceNr][SIchannelNr] <= 0) {
				// Throw in some help for debugging purpose...
				DWORD SIchanHandle[300]; 
				char SIdeviceName[50];
				char cUnit[17];
				int SIrealChannelNr;

				printLog(LEVEL_FATAL, "Error : cannot determine handle of channel name '%s' for device %d\n", SIchannelNames[SIchannelNr], SIdeviceHandles[SIdeviceNr]);
				int SIrealChannelCount = GetChannelHandlesEx(SIdeviceHandles[SIdeviceNr], SIchanHandle, 300, SPOTCHANNELS);
				GetDeviceName(SIdeviceHandles[SIdeviceNr], SIdeviceName, sizeof(SIdeviceName)-1);
				lightLog(LEVEL_ERROR, "Here are the %d available SI channels for device '%s':\n", SIrealChannelCount, SIdeviceName);
				lightLog(LEVEL_ERROR, "---------------------------------------\n");
				lightLog(LEVEL_ERROR, "|   SI Channel name   |   SI Channel Unit   |\n");
				lightLog(LEVEL_ERROR, "---------------------------------------\n");
				for(SIrealChannelNr = 0; SIrealChannelNr < SIrealChannelCount; SIrealChannelNr++)
				{
					GetChannelName(SIchanHandle[SIrealChannelNr], SIchanName, sizeof(SIchanName)-1);
					cUnit[0]=0;
					GetChannelUnit(SIchanHandle[SIrealChannelNr], cUnit, sizeof(cUnit)-1);
					lightLog(LEVEL_ERROR, "| %16s | %-16s |\n", SIchanName, cUnit);
				}
				lightLog(LEVEL_ERROR, "---------------------------------------\n");

				exit(-50);
			}
			GetChannelName(SIchannelHandles[SIdeviceNr][SIchannelNr], SIchanName, sizeof(SIchanName)-1);
			printLog(LEVEL_DETAIL, "SI Channel '%s' has handle %d on device %d\n", SIchannelNames[SIchannelNr], SIchannelHandles[SIdeviceNr][SIchannelNr], SIdeviceHandles[SIdeviceNr]);
		}
	}

	BOOL readOK = true;
	
	// Determine first schedule
	now = time(NULL);
	struct tm *cal = localtime(&now);
	cal->tm_hour = 0;
	cal->tm_min = 0;
	cal->tm_sec = 0;
	dayStart = mktime(cal);
	elapsedSecs = difftime(now, dayStart);

	delay = requestedInterval - ((int)(elapsedSecs) % requestedInterval); 
	printLog(LEVEL_INFO, "Waiting for %d seconds before first trace...\n", delay);
	// Sleep by 1-second interval to allow exit if requested
	for(i=0; (i<delay) && loop; i++) {
		sleep(1);
	}

	printLog(LEVEL_INFO, "Starting loop. Each + below represents a trace successfully inserted in DB:\n", delay);
	while (loop) {
		loopStart = time(NULL);
		
		// Iterate on devices
		for (SIdeviceNr = 0; SIdeviceNr < SIdeviceCount; SIdeviceNr++) {

            // Iterate on channels for this device
			for (SIchannelNr=0; SIchannelNr<SIchannelCount; SIchannelNr++) {
				res = GetChannelValue(SIchannelHandles[SIdeviceNr][SIchannelNr], SIdeviceHandles[SIdeviceNr], &value, textValue, sizeof(textValue)-1, maxChannelValueAge);
				if (res == 0) {
					if (strlen(textValue) == 0) {
						sprintf(textValue, "%.6f", value);
					}
				}
				else {                    
                    // means connection with at least one device was lost. 
                    if (res != -3) {
                        printLog(LEVEL_WARNING, "Error reading channel value....error code=%d\n", res);
                    }
                    readOK = false;
                    loop = false;
					break;
				}
				strcpy(SIchannelValues[SIchannelNr], textValue);
			}
			
			if (readOK) {
				// Insert row
				SIinsertValues(SIchannelCount, SIdbColumns, SIchannelValues);
				lightLog(LEVEL_INFO, "+");
			}
		}

		if (loop) {
            // determine processing duration to schedule next loop turn
            now = time(NULL);
            elapsedSecs = difftime(now, loopStart);

            if (elapsedSecs < requestedInterval) {
                // Compute theoretic start of next loop
                struct tm *cal = localtime(&loopStart);
                cal->tm_sec += requestedInterval;
                loopStart = mktime(cal);
                delay = requestedInterval - elapsedSecs;

                // Must sleep now.
				// Sleep by 1-second interval to allow exit if requested
				for(i=0; (i<delay) && loop; i++) {
					sleep(1);
				}
            }
            else {
                printLog(LEVEL_WARNING, "Warning : Processing took %f seconds. Can't keep up with the %d seconds requested interval. Please choose a higher value.\n", elapsedSecs, requestedInterval);
                loopStart = time(NULL);
            }
        }
	}
    return readOK;
}

/**************************************************************************
* startDeviceLogging
* Start synchronous device detection (blocks until device detection is done)
* then launches logging
**************************************************************************/
void startDeviceLogging(int deviceCount, DWORD deviceHandles[], int channelCount) {
	int iErrorCode, deviceNr;
	char nameBuffer[50];

	printLog(LEVEL_INFO, "Starting detection of %d SB device(s). Please wait...\n", deviceCount);

	/* start searching for devices... */
	BOOL deviceOK = false;
	while (!deviceOK) {
		iErrorCode = DoStartDeviceDetection(deviceCount, TRUE /*blocking*/ );
		switch(iErrorCode) {
			case YE_NOT_ALL_DEVS_FOUND:
				lightLog(LEVEL_INFO, "x");
				sleep(requestedInterval);
				deviceOK = false;	
				break;
			 
			case YE_DEV_DETECT_IN_PROGRESS:
				printLog(LEVEL_FATAL, "\nError: there is already an running SB device detection.\n");
				exit(-40);
		  
			case YE_OK:
				deviceOK = true;	
				break;
			 
			default:
				printLog(LEVEL_FATAL, "\nUnknown error : %d\n", iErrorCode);
				exit(-41);
		}
        
        if (deviceOK) {
			lightLog(LEVEL_INFO, "\n");
            /* get all device handles...*/
            DWORD devCount = GetDeviceHandles(deviceHandles, deviceCount);
                
            if (devCount != deviceCount) {
                printLog(LEVEL_FATAL, "Error : GetDeviceHandles returned %d SB device(s) while we expect %d\n", devCount, deviceCount);
                exit(-42);
            }

            // Debug output
            lightLog(LEVEL_DEBUG, "-------------------------------------------\n");
            lightLog(LEVEL_DEBUG, "SB Device handle | SB Device Name  \n");
            lightLog(LEVEL_DEBUG, "-------------------------------------------\n");
            for (deviceNr = 0; deviceNr < deviceCount; deviceNr++) {
                GetDeviceName(deviceHandles[deviceNr], nameBuffer, sizeof(nameBuffer)-1);
                lightLog(LEVEL_DEBUG, "   %3lu        | '%s'\n", (unsigned long)deviceHandles[deviceNr], nameBuffer);
            }
            lightLog(LEVEL_DEBUG, "-------------------------------------------\n\n");
            
            deviceOK = logValues(deviceCount, deviceHandles, channelCount);
        }
    }
}

/**************************************************************************
* SIstartDeviceLogging
* Start synchronous device detection (blocks until device detection is done)
* then launches logging
**************************************************************************/
void SIstartDeviceLogging(int SIdeviceCount, DWORD SIdeviceHandles[], int SIchannelCount) {
	int SIiErrorCode, SIdeviceNr;
	char SInameBuffer[50];

	printLog(LEVEL_INFO, "Starting detection of %d SI device(s). Please wait...\n", SIdeviceCount);

	/* start searching for devices... */
	BOOL deviceOK = false;
	while (!deviceOK) {
		SIiErrorCode = DoStartDeviceDetection(SIdeviceCount, TRUE /*blocking*/ );
		switch(SIiErrorCode) {
			case YE_NOT_ALL_DEVS_FOUND:
				lightLog(LEVEL_INFO, "-");
				sleep(requestedInterval);
				deviceOK = false;	
				break;
			 
			case YE_DEV_DETECT_IN_PROGRESS:
				printLog(LEVEL_FATAL, "\nError: there is already an running device SI detection.\n");
				exit(-40);
		  
			case YE_OK:
				deviceOK = true;	
				break;
			 
			default:
				printLog(LEVEL_FATAL, "\nUnknown error : %d\n", SIiErrorCode);
				exit(-41);
		}
        
        if (deviceOK) {
			lightLog(LEVEL_INFO, "\n");
            /* get all device handles...*/
            DWORD SIdevCount = GetDeviceHandles(SIdeviceHandles, SIdeviceCount);
                
            if (SIdevCount != SIdeviceCount) {
                printLog(LEVEL_FATAL, "Error : GetDeviceHandles returned %d SI device(s) while we expect %d\n", SIdevCount, SIdeviceCount);
                exit(-42);
            }

            // Debug output
            lightLog(LEVEL_DEBUG, "-------------------------------------------\n");
            lightLog(LEVEL_DEBUG, "SI Device handle | SI Device Name  \n");
            lightLog(LEVEL_DEBUG, "-------------------------------------------\n");
            for (SIdeviceNr = 0; SIdeviceNr < SIdeviceCount; SIdeviceNr++) {
                GetDeviceName(SIdeviceHandles[SIdeviceNr], nameBuffer, sizeof(nameBuffer)-1);
                lightLog(LEVEL_DEBUG, "   %3lu        | '%s'\n", (unsigned long)SIdeviceHandles[SIdeviceNr], nameBuffer);
            }
            lightLog(LEVEL_DEBUG, "-------------------------------------------\n\n");
            
            deviceOK = SIlogValues(SIdeviceCount, SIdeviceHandles, SIchannelCount);
        }
    }
}


/**************************************************************************
usage
**************************************************************************/
void usage(char* program_name) {
	printf("Usage : %s { start | stop | status } [yasdi.ini path]\n", program_name);
    exit(-1);
}

/**************************************************************************
main
**************************************************************************/
int main(int argc, char *argv[])
{
	int driverCount, driverNr;
	DWORD drivers[MAXDRIVERS]; 
	char driverName[30];
	BOOL bOnDriverOnline = FALSE; //Is at least one driver online?
	char *iniFile;	
    int logLevel;
	FILE *logfile;

	if ( argc < 2 ) {        
		usage(argv[0]);
    }
	else {
		// Parse start/stop command
		if (stricmp(argv[1], "stop") == 0 || stricmp(argv[1], "status") == 0) {
			// Send signal to running process
			// Retrieve PID
			char pid[200];
			FILE *fp; 
			fp = fopen(LOCK_FILE, "r");
			if(!fp) {
				printf("No daemon seems to be running right now. Lock file '%s' not found !\n", LOCK_FILE); 
				exit(-1);
			}
			if (fgets(pid, sizeof(pid), fp) != NULL) {
				kill(atoi(pid), (stricmp(argv[1], "stop") == 0)?SIGTERM:SIGUSR1);
				printf("Signal %d sent to process %sPlease check log file '%s'\n", (stricmp(argv[1], "stop") == 0)?SIGTERM:SIGUSR1, pid, LOG_FILE);
			}
			fclose(fp);
			exit(0);
		}
		if (stricmp(argv[1], "start") != 0) {
			usage(argv[0]);
		}
	}	
	
	// yasdi.ini file retrieval and check
	if ( argc > 2 )
    {        
        iniFile=argv[2];
    }
	else {
		char path[1024];
        if (getcwd(path, sizeof(path)) != NULL) {
			strcat(path, "/");
			strcat(path, YASDI_INI);
			iniFile=path;
		}
        else {		
            printf("Error computing current folder\n");
			exit(-1);
		}
	}	
	if( access( iniFile, F_OK ) == -1 ) {
		usage(argv[0]);
	}

	
	
	
	//---------------------------------
    // From here on, we run as a daemon
    //---------------------------------
	printf("Daemon starting. Logging to '%s'...\n", LOG_FILE);
 	daemonize();	
	

	

  	// Read config params
	logLevel = GetPrivateProfileInt_("Log", "level", 3, iniFile);
	requestedInterval = GetPrivateProfileInt_("Params", "requestedInterval", 300, iniFile);
    maxChannelValueAge = GetPrivateProfileInt_("Params", "maxChannelValueAge", 5, iniFile);

	logfile=fopen(LOG_FILE, "a");
    initLog(logLevel, logfile);
    printLog(LEVEL_DETAIL, "SMySqLogger was started\n");

	/* init Yasdi- and Yasdi-Master-Library */
	if (0 > yasdiMasterInitialize(iniFile, &driverCount)) {
		printLog(LEVEL_FATAL, "ERROR: ini file '%s' was not found or is unreadable!\n", iniFile);
		exit(-1);
	}

	/* get List of all supported drivers...*/
	driverCount = yasdiMasterGetDriver(drivers, MAXDRIVERS);

	/* Switch all drivers online (you should only do one of them online!)...*/
	for(driverNr = 0; driverNr < driverCount; driverNr++) {
		/* The name of the driver */
		yasdiGetDriverName(drivers[driverNr], driverName, sizeof(driverName));

		printLog(LEVEL_INFO, "Switching on driver '%s'... ", driverName);

		if (yasdiSetDriverOnline(drivers[driverNr])) {
			lightLog(LEVEL_INFO, "Done\n");
			bOnDriverOnline = TRUE;
		}
		else {
			lightLog(LEVEL_FATAL, "Failure ! Please check config file\n");
			exit(-2);
		}
	}
	lightLog(LEVEL_INFO, "\n");
   
	//Check that at least one driver is online...
	if (FALSE == bOnDriverOnline) {
		printLog(LEVEL_FATAL, "Error: No drivers are online! YASDI can't communicate with devices!\n");
		exit(-3);
	}
	
	// Check arrays consistency 
	int channelCount = checkArrayConsistency();
	int SIchannelCount = SIcheckArrayConsistency();
	
	// Init DB
	dbInit(iniFile, channelCount, dbColumns, dbTypes);
	SIdbInit(iniFile, SIchannelCount, SIdbColumns, SIdbTypes);

	// Autodetect SB devices when started
    int deviceCount = GetPrivateProfileInt_("Plant", "deviceCount", -1, iniFile);
	if (deviceCount == -1) {
		printLog(LEVEL_FATAL, "Error: 'deviceCount=xx' must be specified under section '[Plant]' of %s !\n", iniFile);
		exit(-4);
	}
	
	// Autodetect SI devices when started
    int SIdeviceCount = GetPrivateProfileInt_("Plant", "SIdeviceCount", -1, iniFile);
	if (SIdeviceCount == -1) {
		printLog(LEVEL_FATAL, "Error: 'SIdeviceCount=xx' must be specified under section '[Plant]' of %s !\n", iniFile);
		exit(-4);
	}

   	DWORD deviceHandles[deviceCount];
	DWORD SIdeviceHandles[SIdeviceCount];


	/* Detect SB devices and start logging */
	startDeviceLogging(deviceCount, deviceHandles, channelCount);
	
	/* Detect SI devices and start logging */
	SIstartDeviceLogging(SIdeviceCount, SIdeviceHandles, SIchannelCount);


	
	/* Shutdown all yasdi drivers... */
	for (driverNr = 0; driverNr < driverCount; driverNr++) {
		yasdiGetDriverName(drivers[driverNr], driverName, sizeof(driverName));
		printLog(LEVEL_INFO, "Switching off driver '%s'...\n", driverName);
		yasdiSetDriverOffline(drivers[driverNr]);
	}
	
	/* Shutdown DB */
    printLog(LEVEL_DETAIL, "Shutting down DB...\n");
	dbShutdown();

	/* Shutdown YASDI */
    printLog(LEVEL_DETAIL, "Shutting down Yasdi...\n");
	yasdiMasterShutdown();

	/* Cleanup */
	remove(LOCK_FILE);	
	
    printLog(LEVEL_DETAIL, "Closing log and exiting.\n");
	fclose(logfile);

	return 0;
}
