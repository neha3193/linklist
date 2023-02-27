port=5541
line="wjkjkskdjas"
	 while [ $port -lt 5547 ]
		do  
	 	 	echo " echo $line | my_nc -u 127.0.0.1 $port"
	 	 	 echo $line | nc -u 127.0.0.1 $port &
				 port=`expr $port + 1`
		done


 
