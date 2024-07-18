SOURCE_FILES = vs_speaker.c gsm.c gsm_utilities.c app_debug.c app_aes.c base64.c app_serial.c cJSON.c min.c lwrb.c app_md5.c
DEFINE += -DDEBUG_NUMBER_OF_DEBUG_PORT=3 -DAPP_DEBUG_HAS_FLOAT=1
PARRENT_DIRS = ../

all:
	${OPEN_WRT_GCC_DIR}/mipsel-openwrt-linux-gcc $(SOURCE_FILES) -L../ -l PahoMqttC -I../ -I./ -o vs_speaker -lpthread -ldl ${DEFINE}
clean:
	rm -rf ${PARRENT_DIRS}/*.o ${PARRENT_DIRS}/*.a
	rm -rf *.o
	rm -rf vs_speaker