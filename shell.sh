#!/bin/bash

echo " Reading configuration file $1 "

conf_file=$1

while IFS= read -r line; do
    line=${line%%#*}
    case $line in
        *=*)
           var=${line%%=*}
           case $var in
                *[!A-Z_a-z]*)
                echo "Warning: invalid variable name $var ignored" >&2
                continue;;
           esac
           if eval '[ -n "${'$var'+1}" ]'; then
                echo "Warning: variable $var already set,redefinition ignored" >&2
                continue
           fi
           line=${line#*=}
           eval
           $var='"$line"'
    esac
  done
     <"$conf_file"

     echo "Current values after reading configuration file"

     echo " Server IP
     = $SRV_IP
     Database IP
     = $DB_IP
     Database
     Port =
     $DB_PORT"

     exit 0
