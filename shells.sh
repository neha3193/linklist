#!/bin/bash
while true; do 
nc -lu 16523| (read line; 
        echo $line
        echo $line >> udp16523.log
        )
        sleep 1s;
done
