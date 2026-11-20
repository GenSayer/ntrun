@echo off
rem cmdtest.bat one "two words" three: a test of the command processor
echo 1 arguments [%1] [%2] [%3]
set NAME=value
echo 2 variable %NAME% and 100%%
if "%1"=="one" echo 3 if ==
if not "%1"=="two" echo 4 if not
if exist cmdtest.bat echo 5 if exist
if not exist nosuch.fil echo 6 if not exist
echo text> t1.tmp
echo more>> t1.tmp
type t1.tmp
copy t1.tmp t2.tmp > nul
copy t1.tmp+t2.tmp t3.tmp > nul
type t3.tmp
for %%f in (t1.tmp t2.tmp) do echo 7 for %%f
for %%f in (t?.tmp) do echo 8 pattern %%f
if exist t3.tmp (echo 9 group) & echo 9 after
goto skip
echo not here
:skip
echo 10 goto
shift
echo 11 shift [%1]
echo 12 one& echo 12 two
type nosuch.fil > nul 2>&1 || echo 13 or
type t1.tmp > nul && echo 14 and
md sub.tmp
if exist sub.tmp echo 15 md
ren t3.tmp t4.tmp
if exist t4.tmp echo 16 ren
copy t4.tmp sub.tmp > nul
if exist sub.tmp\t4.tmp echo 17 copy to a folder
del sub.tmp\t4.tmp
rd sub.tmp
if not exist sub.tmp echo 18 rd
del t1.tmp t2.tmp t4.tmp
if not exist t?.tmp echo 19 del
if errorlevel 1 echo not here
cmd /c type nosuch.fil > nul 2>&1
if errorlevel 1 echo 20 errorlevel 1
if not errorlevel 2 echo 21 not errorlevel 2
set NAME=
if "%NAME%"=="" echo 22 set to nothing
echo 23 a caret ^& and "a quoted & sign"
echo.
echo 24 done
