// Unit tests for Custom Capes Resampler v2
// Requirements:
//  • Dynamic Resolution Detection: W×H dynamically
//  • Bilinear Resampling: any weird size -> 64×32 UV Map
//  • Smart Crop & Aspect Ratio: preserve aspect, avoid stretch, auto-fill edges
//  • Must support: 22×23, 88×92, 176×184, 704×736, 736×797 and any custom size
//
// Build: g++ -std=c++20 -I src tests/customcapes_resampler_v2_test.cpp -o /tmp/resampler_v2_test && /tmp/resampler_v2_test

#include "modules/player/customcapes_files.hpp"
#include <cstdio>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>

namespace cc = customcapes;

int g_fail = 0;
void check(bool ok, const std::string& msg){
    if(ok) std::printf("  ok   %s\n", msg.c_str());
    else { std::printf("  FAIL %s\n", msg.c_str()); ++g_fail; }
}

bool samePixel(const std::vector<std::uint8_t>& img, uint32_t ax, uint32_t ay, uint32_t bx, uint32_t by){
    size_t a = (static_cast<size_t>(ay)*cc::kCapeWidth + ax)*4u;
    size_t b = (static_cast<size_t>(by)*cc::kCapeWidth + bx)*4u;
    return img[a]==img[b] && img[a+1]==img[b+1] && img[a+2]==img[b+2] && img[a+3]==img[b+3];
}

int main(){
    std::printf("=== Resampler v2: Dynamic Resolution Detection ===\n");
    // Test that W×H are read dynamically, not assumed fixed
    std::vector<std::pair<uint32_t,uint32_t>> dynamicSizes = {{1,1},{2,3},{10,16},{22,23},{64,32},{128,64},{512,512},{4096,4096}};
    for(auto [w,h] : dynamicSizes){
        std::vector<uint8_t> src(static_cast<size_t>(w)*h*4u, 100);
        for(size_t i=0;i<src.size();i+=4){ src[i+3]=255; }
        auto out = cc::resampleToCape(src.data(), w, h);
        check(out.size()==cc::kCapeWidth*cc::kCapeHeight*4u,
              "dynamic detection " + std::to_string(w) + "x" + std::to_string(h) + " -> 64x32");
    }

    std::printf("\n=== Resampler v2: Bilinear Resampling ===\n");
    {
        // Create 2x2 with distinct colors, upscale to 10x16 back face
        // Nearest neighbor would give blocky 4 colors, bilinear gives smooth blend
        std::vector<uint8_t> src(2*2*4);
        src[0]=0; src[1]=0; src[2]=0; src[3]=255;
        src[4]=255; src[5]=0; src[6]=0; src[7]=255;
        src[8]=0; src[9]=255; src[10]=0; src[11]=255;
        src[12]=0; src[13]=0; src[14]=255; src[15]=255;
        auto out = cc::resampleToCape(src.data(), 2, 2);
        // Sample center pixel of back face
        size_t center = (static_cast<size_t>(cc::kCapeBackY + cc::kCapeBackHeight/2)*cc::kCapeWidth + cc::kCapeBackX + cc::kCapeBackWidth/2)*4u;
        bool blended = out[center+0]>0 && out[center+0]<255 && out[center+1]>0 && out[center+2]>0;
        check(blended, "bilinear produces intermediate blend (not nearest neighbor blocky)");
        // Verify bilinearSample helper exists and works
        uint8_t sampled[4];
        cc::bilinearSample(src.data(), 2, 2, 0.5, 0.5, sampled);
        bool midBlend = sampled[0]>0 && sampled[0]<255;
        check(midBlend, "bilinearSample helper produces blend at 0.5,0.5");
    }
    {
        // Gradient test: bilinear should be smooth, diff between adjacent pixels small
        uint32_t W=22, H=23;
        std::vector<uint8_t> src(static_cast<size_t>(W)*H*4u);
        for(uint32_t y=0;y<H;++y) for(uint32_t x=0;x<W;++x){
            size_t i=(static_cast<size_t>(y)*W+x)*4u;
            src[i]= static_cast<uint8_t>((x*255)/(W-1));
            src[i+1]= static_cast<uint8_t>((y*255)/(H-1));
            src[i+2]=128; src[i+3]=255;
        }
        auto out = cc::resampleToCape(src.data(), W, H);
        bool smooth=true;
        for(uint32_t y=0;y<cc::kCapeBackHeight-1;++y){
            for(uint32_t x=0;x<cc::kCapeBackWidth;++x){
                size_t i1=(static_cast<size_t>(cc::kCapeBackY+y)*cc::kCapeWidth+cc::kCapeBackX+x)*4u;
                size_t i2=(static_cast<size_t>(cc::kCapeBackY+y+1)*cc::kCapeWidth+cc::kCapeBackX+x)*4u;
                int diff = std::abs((int)out[i1+1]-(int)out[i2+1]);
                if(diff>60) smooth=false;
            }
        }
        check(smooth, "bilinear smooth vertical gradient for 22x23");
    }

    std::printf("\n=== Resampler v2: Smart Crop & Aspect Ratio ===\n");
    {
        // Source aspect 2.0 (wider) vs target 0.625, should crop width centrally
        // Create image with red left edge, blue right edge, green center
        // After smart crop (cover), center should remain green, not stretched red/blue
        uint32_t W=20, H=10; // aspect 2.0
        std::vector<uint8_t> src(static_cast<size_t>(W)*H*4u, 0);
        for(uint32_t y=0;y<H;++y) for(uint32_t x=0;x<W;++x){
            size_t i=(static_cast<size_t>(y)*W+x)*4u;
            if(x<2) { src[i]=255; src[i+1]=0; src[i+2]=0; } // red border
            else if(x>=W-2) { src[i]=0; src[i+1]=0; src[i+2]=255; } // blue border
            else { src[i]=0; src[i+1]=255; src[i+2]=0; } // green center
            src[i+3]=255;
        }
        auto out = cc::resampleToCape(src.data(), W, H);
        // Back face should be mostly green (center cropped), not red/blue stretched
        int greenCount=0, redCount=0;
        for(uint32_t y=0;y<cc::kCapeBackHeight;++y) for(uint32_t x=0;x<cc::kCapeBackWidth;++x){
            size_t i=(static_cast<size_t>(cc::kCapeBackY+y)*cc::kCapeWidth+cc::kCapeBackX+x)*4u;
            if(out[i+1]==255 && out[i]==0 && out[i+2]==0) greenCount++;
            if(out[i]==255 && out[i+1]==0) redCount++;
        }
        check(greenCount> redCount, "smart crop preserves center (green) for wide source, avoids stretch");
    }
    {
        // Tall source: aspect 0.2 (taller than target)
        uint32_t W=10, H=50; // aspect 0.2 <0.625
        std::vector<uint8_t> src(static_cast<size_t>(W)*H*4u, 0);
        for(uint32_t y=0;y<H;++y) for(uint32_t x=0;x<W;++x){
            size_t i=(static_cast<size_t>(y)*W+x)*4u;
            if(y<2) { src[i]=255; src[i+1]=0; src[i+2]=0; }
            else if(y>=H-2) { src[i]=0; src[i+1]=0; src[i+2]=255; }
            else { src[i]=0; src[i+1]=255; src[i+2]=0; }
            src[i+3]=255;
        }
        auto out = cc::resampleToCape(src.data(), W, H);
        int greenCount=0;
        for(uint32_t y=0;y<cc::kCapeBackHeight;++y) for(uint32_t x=0;x<cc::kCapeBackWidth;++x){
            size_t i=(static_cast<size_t>(cc::kCapeBackY+y)*cc::kCapeWidth+cc::kCapeBackX+x)*4u;
            if(out[i+1]==255) greenCount++;
        }
        check(greenCount>0, "smart crop preserves center for tall source");
    }

    std::printf("\n=== Resampler v2: Required Custom Sizes ===\n");
    std::vector<std::pair<uint32_t,uint32_t>> required = {{22,23},{88,92},{176,184},{704,736},{736,797}};
    for(auto [W,H] : required){
        std::vector<uint8_t> src(static_cast<size_t>(W)*H*4u);
        for(uint32_t y=0;y<H;++y) for(uint32_t x=0;x<W;++x){
            size_t i=(static_cast<size_t>(y)*W+x)*4u;
            src[i]= (x*255)/(W-1);
            src[i+1]= (y*255)/(H-1);
            src[i+2]= 128;
            src[i+3]= 255;
        }
        auto out = cc::resampleToCape(src.data(), W, H);
        bool sizeOk = out.size()==cc::kCapeWidth*cc::kCapeHeight*4u;
        bool hasContent=false, alphaOk=true;
        for(uint32_t y=0;y<cc::kCapeBackHeight;++y) for(uint32_t x=0;x<cc::kCapeBackWidth;++x){
            size_t i=(static_cast<size_t>(cc::kCapeBackY+y)*cc::kCapeWidth+cc::kCapeBackX+x)*4u;
            if(out[i+3]!=0) hasContent=true;
            if(out[i+3]!=255) alphaOk=false;
        }
        bool edgesOk=true;
        for(uint32_t x=0;x<cc::kCapeBackWidth;++x){
            edgesOk &= samePixel(out, cc::kCapeTopX+x, cc::kCapeTopY, cc::kCapeBackX+x, cc::kCapeBackY);
            edgesOk &= samePixel(out, cc::kCapeBottomX+x, cc::kCapeBottomY, cc::kCapeBackX+x, cc::kCapeBackY+cc::kCapeBackHeight-1);
        }
        bool frontUniform=true;
        size_t firstFront=(static_cast<size_t>(cc::kCapeFrontY)*cc::kCapeWidth+cc::kCapeFrontX)*4u;
        for(uint32_t y=0;y<cc::kCapeBackHeight;++y) for(uint32_t x=0;x<cc::kCapeBackWidth;++x){
            size_t i=(static_cast<size_t>(cc::kCapeFrontY+y)*cc::kCapeWidth+cc::kCapeFrontX+x)*4u;
            if(out[i]!=out[firstFront] || out[i+1]!=out[firstFront+1] || out[i+2]!=out[firstFront+2]) frontUniform=false;
        }
        check(sizeOk, std::to_string(W)+"x"+std::to_string(H)+" -> 64x32");
        check(hasContent, std::to_string(W)+"x"+std::to_string(H)+" back face has content");
        check(alphaOk, std::to_string(W)+"x"+std::to_string(H)+" alpha preserved");
        check(edgesOk, std::to_string(W)+"x"+std::to_string(H)+" edge strips auto-filled from edge colors");
        check(frontUniform, std::to_string(W)+"x"+std::to_string(H)+" front flat lining");
        check(cc::hasElytraArtwork(out), std::to_string(W)+"x"+std::to_string(H)+" elytra generated");
    }

    std::printf("\n=== Resampler v2: Thickness Auto-Fill ===\n");
    {
        uint32_t W=22,H=23;
        std::vector<uint8_t> src(static_cast<size_t>(W)*H*4u);
        for(uint32_t y=0;y<H;++y) for(uint32_t x=0;x<W;++x){
            size_t i=(static_cast<size_t>(y)*W+x)*4u;
            src[i]= static_cast<uint8_t>(x*10);
            src[i+1]= static_cast<uint8_t>(y*10);
            src[i+2]=200; src[i+3]=255;
        }
        auto out = cc::resampleToCape(src.data(), W, H);
        bool topOk=true, bottomOk=true, sideOk=true;
        for(uint32_t x=0;x<cc::kCapeBackWidth;++x){
            topOk &= samePixel(out, cc::kCapeTopX+x, cc::kCapeTopY, cc::kCapeBackX+x, cc::kCapeBackY);
            bottomOk &= samePixel(out, cc::kCapeBottomX+x, cc::kCapeBottomY, cc::kCapeBackX+x, cc::kCapeBackY+cc::kCapeBackHeight-1);
        }
        for(uint32_t y=0;y<cc::kCapeBackHeight;++y){
            sideOk &= samePixel(out, cc::kCapeSideRightX, cc::kCapeSideY+y, cc::kCapeBackX, cc::kCapeBackY+y);
            sideOk &= samePixel(out, cc::kCapeSideLeftX, cc::kCapeSideY+y, cc::kCapeBackX+cc::kCapeBackWidth-1, cc::kCapeBackY+y);
        }
        check(topOk && bottomOk && sideOk, "thickness strips auto-filled from image edge colors");
    }

    std::printf("\n%s\n", g_fail==0?"all resampler v2 tests passed":"SOME TESTS FAILED");
    return g_fail==0?0:1;
}
