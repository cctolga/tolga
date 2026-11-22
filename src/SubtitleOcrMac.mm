// Text recognition with Apple's Vision framework (macOS 10.15+; languages besides English
// come with newer systems). Nothing to download or ship.
#include "SubtitleOcr.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <cctype>
#include <cstdio>
#include <map>

namespace {

// ISO 639-2 (as in Matroska files) -> the language Vision wants
NSString* visionLanguage(const std::string& lang) {
    static const std::map<std::string, const char*> names = {
        {"eng", "en-US"}, {"en", "en-US"}, {"fre", "fr-FR"}, {"fra", "fr-FR"}, {"fr", "fr-FR"},
        {"ger", "de-DE"}, {"deu", "de-DE"}, {"de", "de-DE"}, {"spa", "es-ES"}, {"es", "es-ES"},
        {"ita", "it-IT"}, {"it", "it-IT"}, {"por", "pt-BR"}, {"pt", "pt-BR"}, {"tur", "tr-TR"}, {"tr", "tr-TR"},
        {"rus", "ru-RU"}, {"ru", "ru-RU"}, {"ukr", "uk-UA"}, {"uk", "uk-UA"}, {"dut", "nl-NL"}, {"nld", "nl-NL"},
        {"nl", "nl-NL"}, {"pol", "pl-PL"}, {"pl", "pl-PL"}, {"chi", "zh-Hans"}, {"zho", "zh-Hans"}, {"zh", "zh-Hans"},
        {"jpn", "ja-JP"}, {"ja", "ja-JP"}, {"kor", "ko-KR"}, {"ko", "ko-KR"}, {"swe", "sv-SE"}, {"sv", "sv-SE"},
        {"dan", "da-DK"}, {"da", "da-DK"}, {"nor", "no-NO"}, {"nob", "no-NO"}, {"no", "no-NO"},
        {"fin", "fi-FI"}, {"fi", "fi-FI"}, {"cze", "cs-CZ"}, {"ces", "cs-CZ"}, {"cs", "cs-CZ"},
        {"rum", "ro-RO"}, {"ron", "ro-RO"}, {"ro", "ro-RO"}, {"hun", "hu-HU"}, {"hu", "hu-HU"},
        {"ara", "ar-SA"}, {"ar", "ar-SA"}, {"vie", "vi-VT"}, {"vi", "vi-VT"}, {"tha", "th-TH"}, {"th", "th-TH"},
        {"ind", "id-ID"}, {"id", "id-ID"}, {"may", "ms-MY"}, {"msa", "ms-MY"}, {"ms", "ms-MY"}};
    std::string key = lang;
    for (char& c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
    const auto it = names.find(key);
    return it == names.end() ? nil : [NSString stringWithUTF8String:it->second];
}

}  // namespace

// Vision has every language it knows built in: nothing to get ready
bool ocrPrepare(const std::string&, const std::function<void(float)>&, const std::atomic<bool>&, std::string&) {
    return true;
}

// Vision runs on the Neural Engine / graphics chip: two at a time keep it busy
int ocrThreads() { return 2; }

bool ocrAvailable() {
    if (@available(macOS 10.15, *)) return true;
    return false;
}

bool ocrRecognize(const std::vector<uint8_t>& gray, int width, int height, const std::string& lang,
                  std::vector<OcrPiece>& pieces) {
    pieces.clear();
    if (@available(macOS 10.15, *)) {
        @autoreleasepool {
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
            CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, gray.data(), gray.size(), nullptr);
            CGImageRef img = CGImageCreate(width, height, 8, 8, width, cs, kCGImageAlphaNone, dp, nullptr, false,
                                           kCGRenderingIntentDefault);
            CGDataProviderRelease(dp);
            CGColorSpaceRelease(cs);
            if (!img) return false;

            VNRecognizeTextRequest* req = [[VNRecognizeTextRequest alloc] init];
            req.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
            req.usesLanguageCorrection = YES;
            static std::atomic<bool> logged{false};
            if (NSString* want = visionLanguage(lang)) {
                NSError* err = nil;
                NSArray<NSString*>* supported = [req supportedRecognitionLanguagesAndReturnError:&err];
                // Vision lists some languages without the region ("tr-TR" / "tr")
                NSString* use = nil;
                for (NSString* s in supported)
                    if ([s isEqualToString:want] ||
                        [[s componentsSeparatedByString:@"-"].firstObject
                            isEqualToString:[want componentsSeparatedByString:@"-"].firstObject]) {
                        use = s;
                        break;
                    }
                if (use) req.recognitionLanguages = @[ use ];
                if (!logged.exchange(true)) {
                    std::fprintf(stderr, "[ocr] language %s -> %s\n", lang.c_str(),
                                 use ? use.UTF8String : "not supported here, using the default");
                }
            }
            VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:img options:@{}];
            NSError* error = nil;
            const BOOL ok = [handler performRequests:@[ req ] error:&error];
            CGImageRelease(img);
            if (!ok) return false;

            for (VNRecognizedTextObservation* obs in req.results) {
                VNRecognizedText* best = [obs topCandidates:1].firstObject;
                if (!best || best.string.length == 0) continue;
                const CGRect b = obs.boundingBox;  // 0..1, origin at the bottom left
                OcrPiece p;
                p.text = best.string.UTF8String;
                p.x0 = float(CGRectGetMinX(b) * width);
                p.x1 = float(CGRectGetMaxX(b) * width);
                p.y0 = float((1.0 - CGRectGetMaxY(b)) * height);
                p.y1 = float((1.0 - CGRectGetMinY(b)) * height);
                pieces.push_back(std::move(p));
            }
            return !pieces.empty();
        }
    }
    return false;
}
