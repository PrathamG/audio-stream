ESP-IDF v5.0.0  
ESP-ADF v2.6

Checkout  the git tags for IDF and ADF
git describe --tags

git checkout v5.0

Update the ESP-IDF extension to use version installed on the syste,

Update the path of ESP-IDF and ADF in extension settings for user's path

Update the extension workspace paths same as user's path including Idf: Python Bin Path

if there is error of libcoexist.a then update git submodule update --init --recursive

if there is error of SSID and API_KEY then https://www.esp32.com/viewtopic.php?t=34385
get the skdconfig from git and paste here for GOOGLE_API_KEY and SSID