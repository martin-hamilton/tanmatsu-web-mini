if (CONFIG_BSP_TARGET_TANMATSU OR CONFIG_BSP_TARGET_KONSOOL OR CONFIG_BSP_TARGET_HACKERHOTEL_2026)
list(APPEND files "tanmatsu_coprocessor.bin")
endif()

idf_build_get_property(idf_target IDF_TARGET)
if("${idf_target}" STREQUAL "esp32p4")
list(APPEND extra_requires esp-hosted-tanmatsu badge-elf)
endif()

idf_component_register(
	SRCS
		"main.c"
		# following items are supplementary to the Tanmatsu template app
		"http_download.c"
		"chakrapetchmedium.c"
		"message_dialog.c"
		"common/display.c"
		"common/theme.c"
		"terminal.c"
		"textedit.c"
		"icons.c"
	PRIV_REQUIRES
		esp_lcd
		fatfs
		nvs_flash
		badge-bsp
		gui
		wpa_supplicant
		esp_https_ota
		wifi-manager
		esp-tls
		lwip
		tanmatsu-wifi
		# following items are supplementary to the Tanmatsu template app
		pax-gfx
		pax-codecs
		custom-certificates
		${extra_requires}
	INCLUDE_DIRS
		"."
)


