// No text recognition on this system yet (Windows: Windows.Media.Ocr, Linux: Tesseract later):
// picture subtitles can't be turned into text here.
#include "SubtitleOcr.h"

bool ocrAvailable() { return false; }

int ocrThreads() { return 1; }

bool ocrPrepare(const std::string&, const std::function<void(float)>&, const std::atomic<bool>&, std::string& error) {
    error = "This computer can't read text from pictures.";
    return false;
}

bool ocrRecognize(const std::vector<uint8_t>&, int, int, const std::string&, std::vector<OcrPiece>& pieces) {
    pieces.clear();
    return false;
}
