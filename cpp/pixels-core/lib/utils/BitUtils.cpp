#include <stdexcept>
#include "utils/BitUtils.h"
#include "BitUtils.h"

std::vector<uint8_t> BitUtils::bitWiseCompactLE(std::vector<bool> values)
{
    return bitWiseCompactLE(values, values.size());
}

std::vector<uint8_t> BitUtils::bitWiseCompactLE(std::vector<bool> values, int length)
{
    std::vector<uint8_t> bitWiseOutput;
    int currBit = 0;
    uint8_t current = 0;

    for (int i = 0; i < length; ++i)
    {
        uint8_t v = (values[i] == true) ? 1 : 0;
        current |= v << currBit;

        currBit++;
        if (currBit == 8)
        {                                     // If we've filled a byte
            bitWiseOutput.push_back(current); // Add the byte to the output
            current = 0;                      // Reset for the next byte
            currBit = 0;                      // Reset bit counter
        }
    }

    if (currBit != 0)
    {
        bitWiseOutput.push_back(current);
    }

    return bitWiseOutput; // Return the compacted byte vector
}

std::vector<uint8_t> BitUtils::bitWiseCompactBE(std::vector<bool> values)
{
    return bitWiseCompactBE(values, values.size());
}

std::vector<uint8_t> BitUtils::bitWiseCompactBE(std::vector<bool> values, int length)
{
    std::vector<uint8_t> bitWiseOutput;
    int bitsLeft = 8;
    uint8_t current = 0;

    for (auto i = 0; i < length; ++i)
    {
        uint8_t v = (values[i] == true) ? 1 : 0;
        bitsLeft--;
        current |= v << bitsLeft;

        if (bitsLeft == 0)
        {
            bitWiseOutput.push_back(current);
            current = 0;
            bitsLeft = 8;
        }
    }

    if (bitsLeft != 8)
    {
        bitWiseOutput.push_back(current);
    }

    return bitWiseOutput;
}

std::vector<uint8_t> BitUtils::bitWiseCompact(bool *values, int length, ByteOrder byteOrder)
{
    if (byteOrder == ByteOrder::PIXELS_BIG_ENDIAN)
    {
        return bitWiseCompactBE(values, length);
    }
    else
    {
        return bitWiseCompactLE(values, length);
    }
}

std::vector<uint8_t> bitWiseCompactLE(bool *values, int length)
{
    if (!values || length < 0)
    {
        throw std::runtime_error("Invalid input: values must not be null and length must be non-negative.");
    }

    std::vector<uint8_t> bitWiseOutput;
    int currBit = 0;
    uint8_t current = 0;

    for (int i = 0; i < length; i++)
    {
        uint8_t v = values[i] ? 1 : 0;
        current |= v << currBit;

        currBit++;
        if (currBit == 8)
        {                                     // If we've filled a byte
            bitWiseOutput.push_back(current); // Add the byte to the output
            current = 0;                      // Reset for the next byte
            currBit = 0;                      // Reset bit counter
        }
    }

    if (currBit != 0)
    {
        bitWiseOutput.push_back(current);
    }

    return bitWiseOutput; // Return the compacted byte vector
}

std::vector<uint8_t> bitWiseCompactBE(bool *values, int length)
{
    if (values == nullptr || length < 0)
    {
        throw std::runtime_error("Invalid input: values must not be null and length must be non-negative.");
    }

    std::vector<uint8_t> bitWiseOutput;
    int bitsLeft = 8;
    uint8_t current = 0;

    for (int i = 0; i < length; i++)
    {
        uint8_t v = values[i] ? 1 : 0;
        bitsLeft--;
        current |= v << bitsLeft;

        if (bitsLeft == 0)
        {
            bitWiseOutput.push_back(current);
            current = 0;
            bitsLeft = 8;
        }
    }

    if (bitsLeft != 8)
    {
        bitWiseOutput.push_back(current);
    }

    return bitWiseOutput;
}

std::vector<uint8_t> BitUtils::bitWiseCompact(std::vector<bool> values, ByteOrder byteOrder)
{
    return bitWiseCompact(values, values.size(), byteOrder);
}

std::vector<uint8_t> BitUtils::bitWiseCompact(std::vector<bool> values, int length, ByteOrder byteOrder)
{
    if (byteOrder == ByteOrder::PIXELS_BIG_ENDIAN)
    {
        return bitWiseCompactBE(values, length);
    }
    else
    {
        return bitWiseCompactLE(values, length);
    }
}
