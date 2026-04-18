#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static void stat(const char *l, const float *a, int n){
    float mn=a[0],mx=a[0]; double s=0,sq=0;
    for(int i=0;i<n;i++){float v=a[i]; if(v<mn)mn=v; if(v>mx)mx=v; s+=v; sq+=(double)v*v;}
    double m=s/n, sd=sqrt(sq/n-m*m);
    printf("  %s n=%d min=%.1f max=%.1f mean=%.1f σ=%.1f\n", l, n, mn, mx, m, sd);
}

int main(){
    ps2204a_device_t *dev;
    if (ps2204a_open(&dev)) { perror("open"); return 1; }
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
    ps2204a_disable_trigger(dev);
    ps2204a_set_siggen(dev, PS_WAVE_SINE, 100000.0f, 1000000);
    float *a = calloc(8064, sizeof(float));
    int n;
    ps2204a_set_timebase(dev, 0, 2000);
    for (int w=0; w<5; w++){
        ps2204a_capture_block(dev, 2000, a, NULL, &n);
        char l[16]; snprintf(l, 16, "tb=0 #%d", w);
        stat(l, a, n);
    }
    printf("\nnow with 500 samples:\n");
    ps2204a_set_timebase(dev, 0, 500);
    for (int w=0; w<5; w++){
        ps2204a_capture_block(dev, 500, a, NULL, &n);
        char l[20]; snprintf(l, 20, "tb=0 N=500 #%d", w);
        stat(l, a, n);
    }
    ps2204a_disable_siggen(dev); ps2204a_close(dev);
    return 0;
}
