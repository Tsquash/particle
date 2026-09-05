#ifndef SPI_BUS_H
#define SPI_BUS_H

#include <Arduino.h>
#include <SPI.h>

extern SPIClass epd_spi;
bool initialize_spi_bus();

#endif