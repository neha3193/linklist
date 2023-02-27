#! /bin/bash

read -p "Enter the Username for Disable:" disable_username
Disable_username="${disable_username}:"
read -p "Enter the mail'id(alias) for remove:" remove_mail

echo '#! /bin/bash' >> filename
echo "sudo sed -i '/${Disable_username}/d' mail.txt " >>filename
#echo "cp mail.txt alias" >>filename
echo "sed -e 's/${remove_mail},//' mail.txt >> alias" >> filename
echo "mv alias mail.txt " >>filename
echo "sed -e 's/${remove_mail}//' mail.txt >> alias" >> filename
echo "mv alias mail.txt" >> filename
chmod +x filename
scp filename sushmita@192.168.15.118:/home/sushmita
run="sudo -S ./filename"

new="ssh sushmita@192.168.15.118  ${run}"
${new}


rm filename

