// Unit tests for the Custom Capes resampler
// Requirements:
//  • Dynamic Resolution Detection: W×H dynamically
//  • Bilinear Resampling: any weird size -> 64×32 UV Map
//  • Aspect handling: preserve aspect, avoid stretch, auto-fill edges
//  • Must support: 22×23, 88×92, 176×184, 704×736, 736×797 and any custom size
//  • Nothing of the artwork may silently disappear: the default (Fit) mode
//    maps the WHOLE image onto the cape face, so the leftmost/rightmost and
//    top/bottom pixels of a 22×23 source all reach the 64×32 canvas.
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

    std::printf("\n=== Resampler: Aspect Handling (explicit Crop mode) ===\n");
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
        auto out = cc::resampleToCape(src.data(), W, H, cc::CapeFitMode::Crop);
        // Back face should be mostly green (center cropped), not red/blue stretched
        int greenCount=0, redCount=0;
        for(uint32_t y=0;y<cc::kCapeBackHeight;++y) for(uint32_t x=0;x<cc::kCapeBackWidth;++x){
            size_t i=(static_cast<size_t>(cc::kCapeBackY+y)*cc::kCapeWidth+cc::kCapeBackX+x)*4u;
            if(out[i+1]==255 && out[i]==0 && out[i+2]==0) greenCount++;
            if(out[i]==255 && out[i+1]==0) redCount++;
        }
        check(greenCount> redCount, "crop mode preserves center (green) for wide source, avoids stretch");
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
        auto out = cc::resampleToCape(src.data(), W, H, cc::CapeFitMode::Crop);
        int greenCount=0;
        for(uint32_t y=0;y<cc::kCapeBackHeight;++y) for(uint32_t x=0;x<cc::kCapeBackWidth;++x){
            size_t i=(static_cast<size_t>(cc::kCapeBackY+y)*cc::kCapeWidth+cc::kCapeBackX+x)*4u;
            if(out[i+1]==255) greenCount++;
        }
        check(greenCount>0, "crop mode preserves center for tall source");
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

    std::printf("\n=== Resampler: Fit keeps ALL of a 22x23 cape (regression) ===\n");
    {
        // Regression guard: the resampler used to center-crop every source
        // that was not exactly 64x32, so a 22x23 cape lost 35% of its pixels
        // (both side edges) before it ever reached the cape face. The artwork
        // below carries a marker band on every edge, so a lost edge shows up
        // directly in the 64x32 canvas.
        const uint32_t W=22, H=23;
        std::vector<uint8_t> src(static_cast<size_t>(W)*H*4u);
        for (uint32_t y=0;y<H;++y) {
            for (uint32_t x=0;x<W;++x) {
                uint8_t* p = &src[(static_cast<size_t>(y)*W+x)*4u];
                if (x < 3)            { p[0]=255; p[1]=0;   p[2]=255; } // magenta left
                else if (x >= W-3)    { p[0]=0;   p[1]=255; p[2]=255; } // cyan right
                else if (y == 0)      { p[0]=255; p[1]=255; p[2]=0;   } // yellow top
                else if (y == H-1)    { p[0]=255; p[1]=128; p[2]=0;   } // orange bottom
                else                  { p[0]=0;   p[1]=180; p[2]=0;   } // green body
                p[3]=255;
            }
        }

        auto out = cc::resampleToCape(src.data(), W, H); // default mode == Fit
        auto at = [&out](uint32_t x, uint32_t y) -> const uint8_t* {
            return &out[(static_cast<size_t>(y)*cc::kCapeWidth + x)*4u];
        };
        auto near = [](const uint8_t* p, int r, int g, int b) {
            return std::abs((int)p[0]-r) <= 24 && std::abs((int)p[1]-g) <= 24 &&
                   std::abs((int)p[2]-b) <= 24;
        };
        const uint32_t midY = cc::kCapeBackY + cc::kCapeBackHeight/2;

        check(near(at(cc::kCapeBackX, midY), 255,0,255),
              "22x23 Fit: LEFT edge band reaches the cape face");
        check(near(at(cc::kCapeBackX + cc::kCapeBackWidth - 1, midY), 0,255,255),
              "22x23 Fit: RIGHT edge band reaches the cape face");

        // Letterbox bands continue the image's own top/bottom edges (both of
        // which are pure red/green here), never transparent or black bars.
        check(at(cc::kCapeBackX + 4, cc::kCapeBackY)[2] == 0 &&
                  at(cc::kCapeBackX + 4, cc::kCapeBackY)[3] == 255,
              "22x23 Fit: top band continues the image's top edge (opaque, no blue)");
        check(at(cc::kCapeBackX + 4, cc::kCapeBackY + cc::kCapeBackHeight - 1)[2] == 0 &&
                  at(cc::kCapeBackX + 4, cc::kCapeBackY + cc::kCapeBackHeight - 1)[3] == 255,
              "22x23 Fit: bottom band continues the image's bottom edge");
        check(samePixel(out, cc::kCapeBackX + 4, cc::kCapeBackY,
                        cc::kCapeBackX + 4, cc::kCapeBackY + 1),
              "22x23 Fit: letterbox band repeats the edge row (no gradient seam)");

        bool opaque = true;
        for (uint32_t y=0;y<cc::kCapeBackHeight;++y)
            for (uint32_t x=0;x<cc::kCapeBackWidth;++x)
                opaque &= at(cc::kCapeBackX + x, cc::kCapeBackY + y)[3] == 255;
        check(opaque, "22x23 Fit: the whole cape face is covered (no transparent gaps)");

        // Aspect is preserved: a 22x23 source (nearly square) must not be
        // stretched over the 10x16 face, so the design keeps a squarer shape
        // than Fill would give it.
        auto fill = cc::resampleToCape(src.data(), W, H, cc::CapeFitMode::Fill);
        check(near(&fill[(static_cast<size_t>(midY)*cc::kCapeWidth + cc::kCapeBackX)*4u], 255,0,255),
              "22x23 Fill: LEFT edge band reaches the cape face");
        {
            // Fill stretches the image over the whole face: the source's top
            // edge row (yellow) colours the FIRST face row, and there is no
            // repeated letterbox band.
            const uint8_t* topRow =
                &fill[(static_cast<size_t>(cc::kCapeBackY)*cc::kCapeWidth + cc::kCapeBackX + 4)*4u];
            check(topRow[2] == 0 && topRow[0] >= 150 && topRow[3] == 255,
                  "22x23 Fill: the image's top edge row is stretched onto the first face row");
            check(!samePixel(fill, cc::kCapeBackX + 4, cc::kCapeBackY,
                             cc::kCapeBackX + 4, cc::kCapeBackY + 1),
                  "22x23 Fill: no letterbox band - the design covers the full face height");
        }

        // Crop is the mode that legitimately discards the sides — asserted so
        // the difference between the modes stays visible.
        auto cropped = cc::resampleToCape(src.data(), W, H, cc::CapeFitMode::Crop);
        auto cat = [&cropped](uint32_t x, uint32_t y) -> const uint8_t* {
            return &cropped[(static_cast<size_t>(y)*cc::kCapeWidth + x)*4u];
        };
        check(!near(cat(cc::kCapeBackX, midY), 255,0,255),
              "22x23 Crop: the side bands are cropped away (old behaviour, opt-in only)");
    }

    std::printf("\n=== Resampler: HD canvas 128x64 is scaled down ===\n");
    {
        // A 2:1 source at canvas scale is a scaled-up 64x32 cape layout, not
        // a logo: it must cover the WHOLE 64x32 canvas, not just the back
        // face (and definitely not its center crop).
        const uint32_t W=128, H=64;
        std::vector<uint8_t> src(static_cast<size_t>(W)*H*4u);
        for (uint32_t y=0;y<H;++y) {
            for (uint32_t x=0;x<W;++x) {
                uint8_t* p = &src[(static_cast<size_t>(y)*W+x)*4u];
                if (x < W/2) { p[0]=200; p[1]=20;  p[2]=20;  } // left half red
                else         { p[0]=20;  p[1]=20;  p[2]=200; } // right half blue
                p[3]=255;
            }
        }
        auto out = cc::resampleToCape(src.data(), W, H);
        auto at = [&out](uint32_t x, uint32_t y) -> const uint8_t* {
            return &out[(static_cast<size_t>(y)*cc::kCapeWidth + x)*4u];
        };
        check(at(0,0)[0] > 150 && at(0,0)[2] < 80, "128x64: canvas top-left is the source's left half");
        check(at(cc::kCapeWidth-1, cc::kCapeHeight-1)[2] > 150 &&
                  at(cc::kCapeWidth-1, cc::kCapeHeight-1)[0] < 80,
              "128x64: canvas bottom-right is the source's right half");
        check(at(cc::kCapeBackX + 4, cc::kCapeBackY + 8)[0] > 150,
              "128x64: the cape back face samples the source's left half (layout kept)");
        check(at(cc::kElytraBackX + 4, cc::kElytraSideY + 8)[2] > 150,
              "128x64: the Elytra area samples the source's right half");
        check(cc::hasElytraArtwork(out), "128x64: authored Elytra pixels are preserved");
    }

    std::printf("\n=== Resampler: fit mode selection helpers ===\n");
    check(cc::capeFitModeFromIndex(0) == cc::CapeFitMode::Fit,  "index 0 -> Fit (default)");
    check(cc::capeFitModeFromIndex(1) == cc::CapeFitMode::Fill, "index 1 -> Fill");
    check(cc::capeFitModeFromIndex(2) == cc::CapeFitMode::Crop, "index 2 -> Crop");
    check(cc::capeFitModeFromIndex(42) == cc::CapeFitMode::Fit, "unknown index clamps to Fit");
    check(cc::capeFitModeFromIndex(-3) == cc::CapeFitMode::Fit, "negative index clamps to Fit");
    check(cc::capeFitIndexFromLabel("fill") == 1, "label lookup is case-insensitive");
    check(cc::capeFitIndexFromLabel("Crop") == 2, "label lookup finds Crop");
    check(cc::capeFitIndexFromLabel("nope") == -1, "unknown label is rejected");
    check(cc::makeLabelRadioValue(1, cc::capeFitLabelList()) == "1,Fit,Fill,Crop",
          "radio value for the menu -> \"1,Fit,Fill,Crop\"");
    {
        int idx = -1; std::string nm;
        cc::parseRadioValue("2,Fit,Fill,Crop", idx, nm);
        check(idx == 2 && nm == "Crop", "radio value round-trips through parseRadioValue");
        cc::parseRadioValue("1", idx, nm);
        check(idx == 1 && nm.empty(), "bare index from the menu parses");
    }

    std::printf("\n%s\n", g_fail==0?"all resampler v2 tests passed":"SOME TESTS FAILED");
    return g_fail==0?0:1;
}
