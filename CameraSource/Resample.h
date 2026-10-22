#pragma once
#include <stdlib.h>
#include <math.h>
// Separable area reduction / bilinear enlargement. No sharpened or invented detail.
struct AxisWeights { int start, count; float* weight; };
static void FreeAxis(AxisWeights* a,int n) {
    if(a) { for(int i=0;i<n;i++) free(a[i].weight); free(a); }
}
static AxisWeights* MakeAxis(int src,int dst) {
    AxisWeights* a=(AxisWeights*)calloc(dst,sizeof(AxisWeights));
    if(!a) return NULL;
    for(int d=0;d<dst;d++) {
        double left,right;
        if(dst<src) {
            left=(double)d*src/dst; right=(double)(d+1)*src/dst;
            a[d].start=(int)floor(left); a[d].count=(int)ceil(right)-a[d].start;
        } else {
            left=((double)d+0.5)*src/dst-0.5;
            if(left<0) left=0; if(left>src-1) left=src-1;
            a[d].start=(int)floor(left); a[d].count=a[d].start+1<src?2:1;
            right=left;
        }
        a[d].weight=(float*)calloc(a[d].count,sizeof(float));
        if(!a[d].weight) { FreeAxis(a,dst); return NULL; }
        for(int k=0;k<a[d].count;k++) {
            if(dst<src) {
                int s=a[d].start+k;
                double lo=left>s?left:s, hi=right<s+1?right:s+1;
                a[d].weight[k]=(float)((hi-lo)/(right-left));
            } else a[d].weight[k]=(float)(k?left-floor(left):1-(left-floor(left)));
        }
    }
    return a;
}
static bool ScalePlane(const unsigned char* src,int sw,int sh,int stride,int step,
                      unsigned char* dst,int dw,int dh,int dstride,int dstep) {
    if(!src || !dst || sw<1 || sh<1 || dw<1 || dh<1 || sw>2048 || sh>2048 || dw>2048 || dh>2048) return false;
    AxisWeights* ax=MakeAxis(sw,dw); AxisWeights* ay=MakeAxis(sh,dh);
    float* tmp=(float*)malloc((size_t)dw*sh*sizeof(float));
    if(!ax || !ay || !tmp) {FreeAxis(ax,dw);FreeAxis(ay,dh);free(tmp);return false;}
    for(int y=0;y<sh;y++) for(int x=0;x<dw;x++) {
        float v=0; const AxisWeights& a=ax[x];
        for(int k=0;k<a.count;k++) v+=src[(size_t)y*stride+(a.start+k)*step]*a.weight[k];
        tmp[(size_t)y*dw+x]=v;
    }
    for(int y=0;y<dh;y++) {
        const AxisWeights& a=ay[y];
        const float* r0=tmp+(size_t)a.start*dw;
        if(a.count<=2) {
            const float* r1=a.count==2?r0+dw:r0;
            const float w0=a.weight[0],w1=a.count==2?a.weight[1]:0;
            for(int x=0;x<dw;x++) {
                int n=(int)(r0[x]*w0+r1[x]*w1+0.5f);
                dst[(size_t)y*dstride+x*dstep]=(unsigned char)(n<0?0:n>255?255:n);
            }
        } else for(int x=0;x<dw;x++) {
            float v=0;
            for(int k=0;k<a.count;k++) v+=r0[(size_t)k*dw+x]*a.weight[k];
            int n=(int)(v+0.5f); dst[(size_t)y*dstride+x*dstep]=(unsigned char)(n<0?0:n>255?255:n);
        }
    }
    FreeAxis(ax,dw);FreeAxis(ay,dh);free(tmp);return true;
}
static bool ScaleNV12(const unsigned char* src,int sw,int sh,unsigned char* dst,int dw,int dh) {
    if((sw|sh|dw|dh)&1) return false;
    return ScalePlane(src,sw,sh,sw,1,dst,dw,dh,dw,1) &&
        ScalePlane(src+(size_t)sw*sh,sw/2,sh/2,sw,2,dst+(size_t)dw*dh,dw/2,dh/2,dw,2) &&
        ScalePlane(src+(size_t)sw*sh+1,sw/2,sh/2,sw,2,dst+(size_t)dw*dh+1,dw/2,dh/2,dw,2);
}
