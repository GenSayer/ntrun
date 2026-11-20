# tools.mak: the message compiler, the resource compiler, the librarian,
# a DLL with its import library, and a program that uses all of them.

all: app.exe

msg.h: msg.mc
	mc msg.mc

app.res: app.rc msg.h
	rc -r app.rc

util.lib: util.obj
	lib -nologo -out:util.lib util.obj

mydll.dll: mydll.obj mydll.def
	link -nologo -dll -def:mydll.def -out:mydll.dll mydll.obj kernel32.lib

app.obj: app.c msg.h
	cl -nologo -c app.c

app.exe: app.obj app.res util.lib mydll.dll
	link -nologo -out:app.exe app.obj app.res util.lib mydll.lib kernel32.lib user32.lib

.c.obj:
	cl -nologo -c $<
