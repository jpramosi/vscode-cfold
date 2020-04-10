#include <iostream>

namespace test { @_0_

void aaa()
{ @_1_
    int a = 0;
    int t = 0;

    if (a == 0)
    { @_2_
        //
        //
    } @_2_

    switch (a)
    { @_3_
        case 0: { @_4_ @_5_
            t = a;
            t = a;
        } @_4_ @_5_
        case 1: @_6_
            t = a;
            t = a; @_6_
        case 2:
        case 3: { @_7_ @_8_
            t = a;
            t = a;
        } @_7_ @_8_
        case 4: @_9_
        { @_10_
            t = a;
        } @_9_ @_10_
        case 5: @_11_
            if (a == 0) { @_12_
              //
              //
            } @_12_
            t = a;
            t = a; @_11_
        case 6: @_13_
            { @_14_
                //
            } @_14_
            t = a;
            t = a;
            switch (a)
            { @_15_
                case 0: @_16_
                    std::cout << "0"; @_16_
                case 1:
                case 2: @_17_
                    std::cout << "0"; @_17_
                case 3: @_18_
                    switch (a)
                    { @_19_
                        case 0: @_20_
                            t = a;
                            t = a; @_20_
                        case 1: @_21_
                            t = a;
                            t = a; @_21_
                        case 2:
                        case 3: @_22_
                            if (a == 0) 
                            { @_23_
                                //
                                //
                            } @_23_
                            t = a;
                            t = a; @_22_
                        case 4: @_24_
                            t = a; @_24_
                        case 5: @_25_
                            t = a;
                            t = a; @_25_
                        case 6: @_26_

                            t = a;
                            t = a;
                            switch (a)
                            { @_27_
                                case 0: @_28_
                                    std::cout << "0"; @_28_
                                case 1:
                                case 2: @_29_
                                    std::cout << "0"; @_29_
                                case 3: @_30_
                                    std::cout << "0"; @_30_
                                case 4: @_31_
                                    std::cout << "0"; @_31_
                            } @_26_ @_27_
                        case 7: @_32_
                            t = a;
                            t = a;
 @_32_
                        case 8: @_33_
                            t = a;

                            t = a;
                            switch (a)
                            { @_34_
                                case 0: @_35_
                                    std::cout << "0"; @_35_
                                case 1:
                                case 2: @_36_
                                    std::cout << "0"; @_36_
                            } @_33_ @_34_
                    } @_18_ @_19_
                case 4: @_37_
                    std::cout << "0"; @_37_
            } @_13_ @_15_
        case 7: @_38_
            t = a;
            t = a;
 @_38_
        case 8: @_39_
            t = a;

            t = a;
            switch (a)
            { @_40_
                case 0: @_41_
                    std::cout << "0"; @_41_
                case 1:
                case 2: @_42_
                    std::cout << "0"; @_42_
            } @_39_ @_40_
    } @_3_


    if (a == 0)
    { @_43_
        //
        //
    } @_43_
} @_1_

} // namespace test @_0_