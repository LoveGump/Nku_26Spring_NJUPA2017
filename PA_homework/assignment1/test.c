# include "stdio.h"
int main(){
    float a = 0.1;
    float b = -0.1;
    float c = 0.0;
    printf("Float(0.1 / 0): %f\n", a / 0);
    printf("Float(-0.1 / 0): %f\n", b / 0);
    printf("Float(0.0 / 0): %f\n", c / 0);

    printf("Float(0.1 / 0): %.20f\n", a / 0);
    printf("Float(-0.1 / 0): %.20f\n", b / 0);
    printf("Float(0.0 / 0): %.20f\n", c / 0);   
    return 0;
}