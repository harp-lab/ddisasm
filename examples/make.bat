IF NOT DEFINED TARGET_ARCH (
    SET TARGET_ARCH=x64
)

IF "%VSCMD_ARG_TGT_ARCH%" NEQ "%TARGET_ARCH%" (
    @CALL "%VCINSTALLDIR%\Auxiliary\Build\vcvarsall.bat" %TARGET_ARCH%
)

nmake %*