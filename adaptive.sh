#!/bin/bash

echo_red()   { printf "\033[1;31m$*\033[m\n"; }
echo_green() { printf "\033[1;32m$*\033[m\n"; }
echo_blue()  { printf "\033[1;34m$*\033[m\n"; }

UDP_IP=10.5.0.10
UDP_PORT=9999
LOG_INTERVAL=200

WFBGS_CFG=/etc/wifibroadcast.cfg

if [ $(id -u) -ne "0" ]; then
	echo "Permission denied"
	echo "sudo $0 $1 $2"
	exit 1
fi

# Base URL for downloading files
#BASE_URL="https://raw.githubusercontent.com/sickgreg/OpenIPC-Adaptive-Link/refs/heads/main"
BASE_URL="https://github.com/SnapDragonfly/OpenIPC-Adaptive-Link/tree/porting_jetson_orin"
CONFIG_PATH="config"


# File names
GS_NAME="alink_gs.py"
TXPROFILE_NAME="$(CONFIG_PATH)/txprofiles.conf"
CONF_NAME="$(CONFIG_PATH)/alink.conf"

# Complete URLs for each file
URL_ALINK_GS="${BASE_URL}/${GS_NAME}"
URL_ALINK_TXPROFILE="${BASE_URL}/${TXPROFILE_NAME}"
URL_ALINK_CONF="${BASE_URL}/${CONF_NAME}"

# drone or gs
isSystem=$(grep -o "NAME=Buildroot" /etc/os-release)

if [ "$1" = "gs" ]; then
	if [ ! -z $isSystem ]; then
		echo_red "Error: It doesn't look like it's an SBC"
		exit 1
	fi

	FILE_NAME=adaptive_link
	FILE=/usr/bin/$FILE_NAME
	FILE_CONF=/etc/$FILE_NAME.conf
	PATH_SERVICE=/etc/systemd/system/$FILE_NAME.service

	if [ "$2" = "install" ]; then
		echo "Installing Adaptive Link"
		
		if [ -f $FILE ];then
			echo_red   "$FILE_NAME is already installed. First, delete the program: '$0 gs remove'"
			exit 1
		fi

		if [ -n "$3" ]; then
			cp $GS_NAME $FILE
			
		else
			wget $URL_ALINK_GS -O $FILE
		fi

		chmod +x $FILE
		
		cat <<EOF | tee $PATH_SERVICE
[Unit]
Description=OpenIPC_AdaptiveLink

[Service]
ExecStart=${FILE} --config ${FILE_CONF}
Type=idle
RemainAfterExit=true

[Install]
WantedBy=multi-user.target
EOF
		echo ""
		echo "Run $FILE --config $FILE_CONF &"
		$FILE --config $FILE_CONF & sleep 5
		echo "kill pid $!"
		kill $!
		
		if [ ! -f $FILE_CONF ]; then	
			echo_red "Error: File ${FILE_CONF} not found" 
			exit 1
		fi
		
		sed -i 's/udp_ip.*/udp_ip = '$UDP_IP'/' $FILE_CONF
		sed -i 's/udp_port.*/udp_port = '$UDP_PORT'/' $FILE_CONF


		isLogInterval=$(grep -o "log_interval" ${WFBGS_CFG})
		if [ -z $isLogInterval ]; then
			echo "$(sed '/\[common\]/a log_interval = '${LOG_INTERVAL}'' $WFBGS_CFG)" > $WFBGS_CFG
		else
			echo "$(sed '/log_interval.*/c log_interval = '${LOG_INTERVAL}'' $WFBGS_CFG)" > $WFBGS_CFG
		fi

		systemctl restart wifibroadcast.service
		systemctl enable $FILE_NAME.service
		systemctl start $FILE_NAME.service
		systemctl status $FILE_NAME.service
		
		echo_green "Configuration file ${FILE_CONF}"
		
	elif [ "$2" = "remove" ]; then
		echo "Removing  Adaptive Link"
		
		systemctl stop $FILE_NAME.service && echo "Wait..." && sleep 3
		systemctl status $FILE_NAME.service
		systemctl disable $FILE_NAME.service
		
		rm -f $FILE_CONF $FILE $PATH_SERVICE
	
	elif [ "$2" = "update" ]; then	
		echo "Updating Adaptive Link"
		
		if [ ! -f $FILE ];then
			echo_red   "$FILE_NAME not installed. To install, use: '$0 gs install'"
			exit 1
		fi
		
		systemctl stop $FILE_NAME.service && echo "Wait..." && sleep 3
		systemctl status $FILE_NAME.service
		
		if [ "$3" = "src" ]; then
			cp $GS_NAME $FILE
		else
			wget $URL_ALINK_GS -O $FILE
		fi
		
		isLogInterval=$(grep -o "log_interval" ${WFBGS_CFG})
		if [ -z $isLogInterval ]; then
			echo "$(sed '/\[common\]/a log_interval = '${LOG_INTERVAL}'' $WFBGS_CFG)" > $WFBGS_CFG
		else
			echo "$(sed '/log_interval.*/c log_interval = '${LOG_INTERVAL}'' $WFBGS_CFG)" > $WFBGS_CFG
		fi

		systemctl restart wifibroadcast.service
		
		systemctl start $FILE_NAME.service 
		systemctl status $FILE_NAME.service
		
		echo_green "The update is complete"
	
	fi
	exit 0

elif [ "$1" = "drone" ]; then
	if [ -z $isSystem ]; then
		echo_red "Error: It doesn't look like it's an Drone"
		exit 1
	fi

	# Check for specific values in the third argument ($3)
	case "$3" in
		*star6b0*)
			echo "star6b0 adaptive link ... ..."
			;;
		*star6e*)
			echo "star6e adaptive link ... ..."
			;;
		*goke*)
			echo "goke adaptive link ... ..."
			;;
		*hisi*)
			echo "hisi adaptive link ... ..."
			;;
		*)
			echo_red "Error: $2 not supported!"
			exit 1  # Exit if none of the conditions are met
			;;
	esac

	RELEASE_PATH="release/$3"
	DRONE_NAME="$(RELEASE_PATH)/ALink42n"
	URL_ALINK_DRONE="${BASE_URL}/${DRONE_NAME}"
	
	TXPROFILE=/etc/txprofiles.conf
	ALINK=/etc/alink.conf
	FILE_NAME=ALink42p
	FILE=/usr/bin/$FILE_NAME
	
	if [ "$2" = "install" ]; then
		echo "Installing Adaptive Link"
		
		if [ -f $FILE ];then
			echo_red "$FILE_NAME is already installed. First, delete the program: '$0 drone remove'"
			exit 1
		fi

		if [ -n "$3" ]; then
			cp $DRONE_NAME $FILE
			cp $TXPROFILE_NAME $TXPROFILE
			cp $CONF_NAME $ALINK
		else
			curl -L -o $FILE $URL_ALINK_DRONE
			curl -L -o $TXPROFILE $URL_TXPROFILE_CONF
			curl -L -o $ALINK $URL_ALINK_CONF
		fi

		chmod +x $FILE
		
		isFile=$(grep -o "$FILE" /etc/rc.local)
		if [ -z $isFile ]; then
			sed -i -e '$i \'$FILE' --ip '$UDP_IP' --port '$UDP_PORT' &' /etc/rc.local
		fi
		
		sed -i 's/tunnel=.*/tunnel=true/' /etc/datalink.conf
		
		# Outputs garbage to the console
		#echo "Starting $FILE"
		#$FILE --ip $UDP_IP --port $UDP_PORT < /dev/null &
		
		echo_green "Installation completed. Restart the system"
			
	elif [ "$2" = "remove" ]; then
		echo "Removing Adaptive Link"
		
		echo "killall $FILE_NAME"
		killall $FILE_NAME && echo "Wait..." && sleep 1
		
		echo "Remove from /etc/rc.local"
		sed -i '/.*'$FILE_NAME'.*/d' /etc/rc.local
		
		
		echo "Remove $FILE $TXPROFILE $ALINK"
		rm -f $FILE $TXPROFILE $ALINK
	
	elif [ "$2" = "update" ]; then	
		echo "Updating Adaptive Link"
		
		if [ ! -f $FILE ];then
			echo_red   "$FILE_NAME not installed. To install, use: '$0 drone install'"
			exit 1
		fi

		echo "killall $FILE_NAME"
		killall $FILE_NAME && echo "Wait..." && sleep 1

		if [ -n "$3" ]; then
			cp $DRONE_NAME $FILE
			cp $TXPROFILE_NAME $TXPROFILE
			cp $CONF_NAME $ALINK
		else
			curl -L -o $FILE $URL_ALINK_DRONE
			curl -L -o $TXPROFILE $URL_TXPROFILE_CONF
			curl -L -o $ALINK $URL_ALINK_CONF
		fi

		echo_green "The update is complete. Restart the system"
	fi
	
	exit 0
fi


cat <<EOF
Usage: $0 gs install|remove|update
       $0 drone install|remove|update [goke|hisi|star6b0|star6e]
EOF


exit 0
