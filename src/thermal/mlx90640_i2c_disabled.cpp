#include <cstdint>

#include "MLX90640_I2C_Driver.h"

// Project two receives EEPROM and frame RAM through NVMEM/V4L2. These stubs
// intentionally prevent the vendored algorithm from opening a second I2C path.
void MLX90640_I2CInit(void) {}

int MLX90640_I2CGeneralReset(void)
{
    return -1;
}

int MLX90640_I2CRead(std::uint8_t, std::uint16_t, std::uint16_t,
                     std::uint16_t *)
{
    return -1;
}

int MLX90640_I2CWrite(std::uint8_t, std::uint16_t, std::uint16_t)
{
    return -1;
}

void MLX90640_I2CFreqSet(int) {}
