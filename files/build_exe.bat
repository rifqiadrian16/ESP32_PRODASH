@echo off
:: Pindah ke folder tempat .bat ini berada (bukan System32)
cd /d "%~dp0"

echo ============================================
echo  Build OTA Updater .exe
echo ============================================
echo.
echo Folder kerja: %cd%
echo.

:: Pastikan ota_updater.py ada di sini
if not exist "ota_updater.py" (
    echo ERROR: File ota_updater.py tidak ditemukan di folder ini!
    echo Pastikan build_exe.bat dan ota_updater.py ada di folder yang SAMA.
    pause
    exit /b 1
)

echo [1/3] Install PyInstaller...
python -m pip install pyinstaller --quiet
if %errorlevel% neq 0 (
    echo ERROR: Python tidak ditemukan.
    pause
    exit /b 1
)

echo.
echo [2/3] Build .exe ^(mungkin 1-2 menit^)...
python -m PyInstaller ^
    --onefile ^
    --windowed ^
    --name "LivinaPro_OTA_Updater" ^
    --icon NONE ^
    ota_updater.py

if %errorlevel% neq 0 (
    echo.
    echo ERROR: Build gagal.
    pause
    exit /b 1
)

echo.
echo [3/3] Selesai!
echo.
echo File .exe ada di folder: %cd%\dist\
echo Nama file             : LivinaPro_OTA_Updater.exe
echo.
pause