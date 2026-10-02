#include "mxl/domain.hpp"

#include <iostream>
#include <string>

// Reads a video flow for a few grains and prints width, height and grain count.
// Usage: mxl-srt-flow-verify <domain-dir> <flow-id> <grains>
int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::cerr << "usage: mxl-srt-flow-verify <domain-dir> <flow-id> <grains>\n";
        return 2;
    }
    using namespace srtgw;
    try
    {
        MxlDomain domain(argv[1], "verify", 1000000000, false);
        MxlVideoReader reader;
        if (!reader.open(domain, argv[2]))
        {
            std::cerr << "flow not found\n";
            return 1;
        }
        auto const format = reader.format();
        int const want = std::atoi(argv[3]);
        int got = 0;
        std::uint64_t index = reader.head() > 2 ? reader.head() - 2 : reader.head();
        for (int i = 0; i < want; ++i)
        {
            std::vector<std::uint8_t> payload;
            bool invalid = false;
            if (reader.read(index, 500000000, payload, &invalid) && !invalid && !payload.empty())
            {
                ++got;
            }
            ++index;
        }
        std::cout << format.width << " " << format.height << " " << (format.interlaced ? "i" : "p") << " " << format.rate.num << "/" << format.rate.den
                  << " grains " << got << "\n";
        return got > 0 ? 0 : 1;
    }
    catch (std::exception const& ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
