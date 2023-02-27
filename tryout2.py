#!/usr/bin/env python3
#
# Shows GOP structure of video file. Useful for checking suitability for HLS and DASH packaging.
# Example:
#
# $ iframe-probe.py myvideo.mp4
# GOP: IPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP 60 CLOSED
# GOP: IPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP 60 CLOSED
# GOP: IPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP 60 CLOSED
# GOP: IPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP 60 CLOSED
# GOP: IPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP 60 CLOSED
# GOP: IPPPPPPPPPPPPPPPPP 18 CLOSED
#
# Key:
#     I: IDR Frame
#     i: i frame
#     P: p frame
#     B: b frame

from __future__ import print_function
import json
import subprocess
import argparse

parser = argparse.ArgumentParser()
parser.add_argument('filename', help='video file to parse')
parser.add_argument('-e', '--ffprobe-exec', dest='ffprobe_exec', 
                    help='ffprobe executable. (default: %(default)s)',
                    default='ffprobe')

args = parser.parse_args()

#if args.Output:
print("Displaying Output as: % s" % args)

def get_frames_metadata(file):
    command = '"{ffexec}" -show_frames -print_format json "{filename}"'.format(ffexec=args.ffprobe_exec, filename=file)
    response_json = subprocess.check_output(command, shell=True, stderr=None)
    frames = json.loads(response_json)["frames"]
    frames_metadata, frames_type, frames_type_bool = [], [], []
    for frame in frames:
        if frame["media_type"] == "video":
            video_frame = json.dumps(dict(frame), indent=4)
            frames_metadata.append(video_frame)
            frames_type.append(frame["pict_type"])
            if frame["pict_type"] == "I":
                frames_type_bool.append(True)
            else:
                frames_type_bool.append(False)
    print(frames_type)
    return frames_metadata, frames_type, frames_type_bool


get_frames_metadata(args.filename)
# counter = 0
# 
# last_frame_number = None
# 
# 
# for gop in gops:
#     for frame in gop.frames:
#         current_frame_number = int(frame.json[u'coded_picture_number'])
#         print (current_frame_number)
#         
#         
#         if last_frame_number is not None:
#             difference = current_frame_number - last_frame_number
#             if difference > 1:
#                 print("Difference is: %s" % difference)
#     
#         counter += 1
#         if counter > 1000:
#             break
#         
#         last_frame_number = int(frame.json[u'coded_picture_number'])
