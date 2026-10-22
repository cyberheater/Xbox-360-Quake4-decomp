@echo off
rem Quake 4 (native renderer) with the CPU profiler switched on.
rem Play normally; when the frame rate dips, keep playing for a few seconds, then quit.
rem Results go to C:\Quake4\profile_session (prof.txt, prof_timeline.csv, q4hle.log).
rem Your controller input is recorded to input_record.txt there, so the session can be replayed.
set Q4_PROF=1
set Q4_TRACE_DIR=C:\Quake4\profile_session
set Q4_INPUT_RECORD=C:\Quake4\profile_session\input_record.txt
if not exist "%Q4_TRACE_DIR%" mkdir "%Q4_TRACE_DIR%"
cd /d "C:\Quake4\rex\q4\out\build\win-amd64-release"
start "" q4hle.exe
