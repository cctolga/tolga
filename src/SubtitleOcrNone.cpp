// No text recognition on this system yet (Windows: Windows.Media.Ocr, Linux: Tesseract later):
// picture subtitles can't be turned into text here.
#include "SubtitleOcr.h"

bool ocrAvailable() { return false; }

bool ocrRecognize(const std::vector<uint8_t>&, int, int, const std::string&, std::vector<OcrPiece>& pieces) {
    pieces.clear();
    return false;
}
