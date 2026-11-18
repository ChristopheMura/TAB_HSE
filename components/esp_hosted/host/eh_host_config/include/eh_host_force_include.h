/* Inclusion forcee par pioarduino dans TOUS les fichiers (y compris le bootloader).
 * On ne charge la configuration esp_hosted que lorsqu'elle existe :
 * pas dans le bootloader, et seulement si la cible C6 est configuree. */
#pragma once
#include "sdkconfig.h"
#if !defined(BOOTLOADER_BUILD) && defined(CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6)
#include "eh_host_port_master_config.h"
#endif