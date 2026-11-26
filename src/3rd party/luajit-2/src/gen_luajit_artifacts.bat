@echo off
setlocal EnableExtensions EnableDelayedExpansion
pushd "%~dp0"

REM Determine DynASM path and platform flags
echo [LuaJIT] Determine DynASM path and if platform is x64
set "DYNASM=%~dp0..\dynasm\dynasm.lua"
if /I "%~1"=="x64" (
  echo [LuaJIT] Platform is x64, set -D P64
  set "DFLAGS=-D WIN -D JIT -D FFI -D P64"
) else (
  echo [LuaJIT] Platform is not x64
  set "DFLAGS=-D WIN -D JIT -D FFI"
)

REM Build minilua if missing
if not exist "host\minilua.exe" (
  echo [LuaJIT] Building host\minilua.exe
  cl /nologo /c /O2 /Ob3 /Oi /Ot /Oy /GT /W3 /fp:precise /MT /GF /GS- /Zi /D_CRT_SECURE_NO_DEPRECATE host\minilua.c || exit /b 1
  link /nologo /OPT:REF /OPT:ICF /out:host\minilua.exe minilua.obj || exit /b 1
)

REM Generate buildvm_arch.h with DynASM
echo [LuaJIT] Running DynASM to generate host\buildvm_arch.h
"host\minilua.exe" "%DYNASM%" -LN %DFLAGS% -o "host\buildvm_arch.h" "vm_x86.dasc" || exit /b 1

REM Build buildvm if missing
if not exist "host\buildvm.exe" (
  echo [LuaJIT] Building host\buildvm.exe
  cl /nologo /c /O2 /Ob3 /Oi /Ot /Oy /GT /W3 /fp:precise /MT /GF /GS- /Zi /I "." /I "..\dynasm" host\buildvm.c host\buildvm_peobj.c host\buildvm_lib.c host\buildvm_asm.c host\buildvm_fold.c || exit /b 1
  link /nologo /OPT:REF /OPT:ICF /out:host\buildvm.exe buildvm.obj buildvm_peobj.obj buildvm_lib.obj buildvm_asm.obj buildvm_fold.obj || exit /b 1
)

REM Generate VM object and header files (a file whose content is the same keeps its timestamp, so lua51.lib and the
REM exes linking it are not rebuilt)
echo [LuaJIT] Generating lj_vm.obj and headers
set "LJLIB_C=lib_base.c lib_math.c lib_bit.c lib_string.c lib_table.c lib_io.c lib_os.c lib_package.c lib_debug.c lib_jit.c lib_ffi.c"
call :update peobj lj_vm.obj "" || exit /b 1
call :update bcdef lj_bcdef.h "%LJLIB_C%" || exit /b 1
call :update ffdef lj_ffdef.h "%LJLIB_C%" || exit /b 1
call :update libdef lj_libdef.h "%LJLIB_C%" || exit /b 1
call :update recdef lj_recdef.h "%LJLIB_C%" || exit /b 1
call :update vmdef jit\vmdef.lua "%LJLIB_C%" || exit /b 1
call :update folddef lj_folddef.h lj_opt_fold.c || exit /b 1

echo [LuaJIT] Generation complete
popd
exit /b 0

REM :update <buildvm mode> <output> <inputs>: generated to <output>.tmp, which replaces <output> only when they differ
:update
host\buildvm.exe -m %1 -o "%~2.tmp" %~3 || exit /b 1
fc /b "%~2.tmp" "%~2" >nul 2>&1
if errorlevel 1 (move /y "%~2.tmp" "%~2" >nul) else (del "%~2.tmp")
exit /b 0
