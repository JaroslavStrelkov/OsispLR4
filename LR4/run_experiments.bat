@echo off
setlocal
if not exist build\LR4.exe (
  echo First compile the project. See README.md
  exit /b 1
)
if not exist data mkdir data
del /q data\results_student104.csv 2>nul
del /q data\events_student104.csv 2>nul
set STUDENT_ID=104
set SEED=11088
set OPERATIONS=720
for %%R in (1 2 3) do (
  for %%L in (global per-record) do (
    for %%D in (uniform biased) do (
      echo Individual run %%R: %%L %%D
      build\LR4.exe --lock %%L --dist %%D --readers 3 --writers 2 --ops %OPERATIONS% --seed %SEED% --timeout 120000 --quiet --csv data\results_student104.csv --events data\events_student104.csv
      if errorlevel 1 echo WARNING: run ended with timeout or invariant error
    )
  )
)
echo Done. Results: data\results_student104.csv
