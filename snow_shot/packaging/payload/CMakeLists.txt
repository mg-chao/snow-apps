cmake_minimum_required(VERSION 4.2)
project(SnowShotInstallerPayload LANGUAGES C CXX)

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/SnowShotInstallerPayload.cmake")
snow_shot_add_installer_payload()
