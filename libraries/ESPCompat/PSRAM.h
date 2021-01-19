#ifndef PSRAM_H
#define PSRAM_H

#ifdef BOARD_HAS_PSRAM
    #define ESP_MALLOC(size) ps_malloc((size))
#else
    #define ESP_MALLOC(size) malloc((size))
#endif

#endif