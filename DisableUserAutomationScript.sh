#! /bin/bash

#MAIL_IP=('192.168.25.206' '192.168.25.207' '192.168.25.208' '192.168.25.230' '192.168.25.235')
MAIL_IP=('192.168.15.222')
SAMBATOOL_PATH="/usr/local/samba/bin/"
read -p "Enter the Username for Disable:" disable_username 
USERLIST_COMMAND="${SAMBATOOL_PATH}samba-tool user list"
DISABLE_COMMAND="${SAMBATOOL_PATH}samba-tool user disable ${disable_username}"

Disable_username="${disable_username}:"
read -p "Enter the mail'id(alias) for remove:" remove_mail 
ARGUMENTCOMMAND="sudo sed -i '/${Disable_username}/d' /etc/aliases; sed -e 's/${remove_mail},//'
/etc/aliases >> aliases;mv aliases /etc/aliases;sed -e 's/${remove_mail}//'
/etc/aliases >> aliases;mv aliases /etc/aliases; sudo newaliases"

list=`${USERLIST_COMMAND}`
SSH_USERNAME="anil"
USERDIRECTORY="/home/PLANETC"
REPLACEOWNERSHIP="chown -R root ${USERDIRECTORY}"
SENDTO=('nehas.sw@planetc.net')

Filename=${disable_username}.txt
touch ${Filename}

printf "Details for ${disable_username}.\n" > ${Filename}
if [[ $list == *${disable_username}* ]]
    then
        echo "User is  there! ${disable_username}";
        ${DISABLE_COMMAND}
        statCodeDisable=$?
        if [ ${statCodeDisable} == 0 ]
        then
           echo "USERID Disabled" >> ${Filename}
        else
           statCodeDisable=1
           echo "USERID not Disabled" >> ${Filename}
        fi
    else
        echo "User is not here!"
        echo "USERID not Disabled(Note: User is not here)" >> ${Filename}
        statCodeDisable=1
fi

for IP in "${MAIL_IP[@]}"
do
    if [ "`ping -c 1 ${IP}`" ]
        then
            echo "ping sucess with ${IP}"
            AliaseRemovalCommand="ssh -p 1022 ${SSH_USERNAME}@${IP} ${ARGUMENTCOMMAND}"
            ${AliaseRemovalCommand}
            statCodeAliaseRemoval=$?
            #echo $?
            if [ ${statCodeAliaseRemoval} == 0 ]
                then
                echo "Success:Removal of Aliase Entry"
                echo "Success:Removal of Aliase Entry" >> ${Filename}
            else
                statCodeAliaseRemoval=1
                echo "Error:Removal of Aliase Entry"
                echo "Error:Removal of Aliase Entry" >> ${Filename}
            fi
            OwnerchangeCommand="ssh -p 1022 ${SSH_USERNAME}@${IP} ${REPLACEOWNERSHIP}"
            ${OwnerchangeCommand}
            statCodeMailDirOwnerChange=$?
            if [ ${statCodeMailDirOwnerChange} == 0 ]
                then
                echo "Success:Mail Directory ownership changed"
                echo "Success:Mail Directory ownership changed" >> ${Filename}
            else
                statCodeMailDirOwnerChange=1
                echo "Error:Mail Directory ownership changed"
                echo "Error:Mail Directory ownership changed" >> ${Filename}
            fi

    else
       echo "Error:ping ${IP}"
    fi
done
for email in "${SENDTO[@]}"
do
    ssmtp ${email} < ${Filename}
done

