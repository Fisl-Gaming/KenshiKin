@echo off
rem Builds the plugin and assembles dist\KenshiKin\ = the folder to copy into [Kenshi]\mods\
setlocal
cd /d "%~dp0"
call plugin\build.bat || exit /b 1
set "D=dist\KenshiKin"
rem Start from an empty folder: it is zipped as is for a release (no stale files).
if exist "%D%" rmdir /s /q "%D%"
mkdir "%D%"
copy /y plugin\out\KenshiKin.dll "%D%\" >nul
copy /y kenshikin.cfg "%D%\" >nul
copy /y LICENSE "%D%\" >nul
copy /y NOTICE.md "%D%\" >nul
copy /y README.md "%D%\" >nul
copy /y README.ru.md "%D%\" >nul
copy /y CHANGELOG.md "%D%\" >nul
copy /y ..\deps\KenshiLib_Examples\HelloWorld\HelloWorld\HelloWorld.mod "%D%\KenshiKin.mod" >nul
> "%D%\RE_Kenshi.json" echo { "Plugins" : [ "KenshiKin.dll" ] }
rem Without RE_Kenshi: loader + KenshiLib (RE_Kenshi 0.3.5's build, GPLv3) + address table of the
rem stock Steam build, named as KenshiLib expects it (see tools\README.md). The game starts the loader
rem through MyGUI: gui\core\core_settings.xml (tools\make_gui_xml.ps1), nothing to run by the player.
copy /y plugin\out\KenshiKin_Loader.dll "%D%\" >nul
copy /y ..\deps\re_release\install\KenshiLib.dll "%D%\" >nul || exit /b 1
mkdir "%D%\rva\RE_Kenshi\RVAs"
copy /y tools\rva\Steam_1.0.68.br "%D%\rva\RE_Kenshi\RVAs\Steam_1.0.65.br" >nul
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\make_gui_xml.ps1" -Dir "%~dp0%D%" || exit /b 1
echo Package ready: %~dp0%D%
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\check_package.ps1" -Dir "%~dp0%D%" || exit /b 1
