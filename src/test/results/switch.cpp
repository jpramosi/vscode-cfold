#include <iostream>

namespace test { @_0_

void aaa()
{ @_1_
    int a = 0;
    int t = 0;

    if (a == 0)
    {
        //
        //
    }

    switch (a)
    {
        case 0: { @_2_
            t = a;
            t = a;
        } @_2_
        case 1: @_3_
            t = a;
            t = a; @_3_
        case 2:
        case 3: { @_4_
            t = a;
            t = a;
        } @_4_
        case 4: @_5_
            t = a; @_5_
        case 5: @_6_
            t = a;
            t = a; @_6_
        case 6: @_7_

            t = a;
            t = a;
            switch (a)
            {
                case 0: @_8_
                    std::cout << "0"; @_8_
                case 1:
                case 2: @_9_
                    std::cout << "0"; @_9_
                case 3: @_10_
                    switch (a)
                    {
                        case 0: @_11_
                            t = a;
                            t = a; @_11_
                        case 1: @_12_
                            t = a;
                            t = a; @_12_
                        case 2:
                        case 3: @_13_
                            t = a;
                            t = a; @_13_
                        case 4: @_14_
                            t = a; @_14_
                        case 5: @_15_
                            t = a;
                            t = a; @_15_
                        case 6: @_16_

                            t = a;
                            t = a;
                            switch (a)
                            {
                                case 0: @_17_
                                    std::cout << "0"; @_17_
                                case 1:
                                case 2: @_18_
                                    std::cout << "0"; @_18_
                                case 3: @_19_
                                    std::cout << "0"; @_19_
                                case 4: @_20_
                                    std::cout << "0"; @_20_
                            } @_16_
                        case 7: @_21_
                            t = a;
                            t = a;
 @_21_
                        case 8: @_22_
                            t = a;

                            t = a;
                            switch (a)
                            {
                                case 0: @_23_
                                    std::cout << "0"; @_23_
                                case 1:
                                case 2: @_24_
                                    std::cout << "0"; @_24_
                            } @_22_
                    } @_10_
                case 4: @_25_
                    std::cout << "0"; @_25_
            } @_7_
        case 7: @_26_
            t = a;
            t = a;
 @_26_
        case 8: @_27_
            t = a;

            t = a;
            switch (a)
            {
                case 0: @_28_
                    std::cout << "0"; @_28_
                case 1:
                case 2: @_29_
                    std::cout << "0"; @_29_
            } @_27_
    }


    if (a == 0)
    {
        //
        //
    }
} @_1_

} // namespace test @_0_