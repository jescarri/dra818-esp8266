#!/bin/bash

MD5sum=$(md5sum build/dra.ino.bin | awk '{print $1}')

curl -v -XPOST --header "Content-Type: multipart/form-data" \
   --header "Accept-Encoding: gzip, deflate" \
   --header "Origin: http://192.168.2.31" \
   --header "Referer: http://192.168.2.31/update" \
   -u ${USER}:${PASSWORD} \
   -F 'firmware=@build/dra.ino.bin' -F 'MD5=ce0370a2c40a88c892cba2b838ee06bd' \
   http://${THING_IP}/update
