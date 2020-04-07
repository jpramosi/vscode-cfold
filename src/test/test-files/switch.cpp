#include <iostream>

namespace test {

void aaa()
{
    int a = 0;
    int t = 0;

    if (a == 0)
    {
        //
        //
    }

    switch (a)
    {
        case 0: {
            t = a;
            t = a;
        }
        case 1:
            t = a;
            t = a;
        case 2:
        case 3: {
            t = a;
            t = a;
        }
        case 4:
            t = a;
        case 5:
            t = a;
            t = a;
        case 6:

            t = a;
            t = a;
            switch (a)
            {
                case 0:
                    std::cout << "0";
                case 1:
                case 2:
                    std::cout << "0";
                case 3:
                    switch (a)
                    {
                        case 0:
                            t = a;
                            t = a;
                        case 1:
                            t = a;
                            t = a;
                        case 2:
                        case 3:
                            t = a;
                            t = a;
                        case 4:
                            t = a;
                        case 5:
                            t = a;
                            t = a;
                        case 6:

                            t = a;
                            t = a;
                            switch (a)
                            {
                                case 0:
                                    std::cout << "0";
                                case 1:
                                case 2:
                                    std::cout << "0";
                                case 3:
                                    std::cout << "0";
                                case 4:
                                    std::cout << "0";
                            }
                        case 7:
                            t = a;
                            t = a;

                        case 8:
                            t = a;

                            t = a;
                            switch (a)
                            {
                                case 0:
                                    std::cout << "0";
                                case 1:
                                case 2:
                                    std::cout << "0";
                            }
                    }
                case 4:
                    std::cout << "0";
            }
        case 7:
            t = a;
            t = a;

        case 8:
            t = a;

            t = a;
            switch (a)
            {
                case 0:
                    std::cout << "0";
                case 1:
                case 2:
                    std::cout << "0";
            }
    }


    if (a == 0)
    {
        //
        //
    }
}

} // namespace test