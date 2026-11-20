
## Whisper wrote Hinglish in Devanagari, so WER showed 100%

- **What we tried:** Whisper small (faster-whisper, CPU, int8, language auto) on our 5 real clips.
- **What happened:** Corpus WER was 100%. Every word counted as wrong, even though the model understood much of the speech.
- **Why:** Our correct text is in Roman letters, and Whisper wrote everything in Devanagari, including English words (for example "order" came out as "अडर"). A word in another script never matches, so no word matched.
- **What this teaches:** Raw WER is not a fair number for Hinglish until both texts are in the same script. We will add script handling, and Character Error Rate next to WER.
- **Other things seen (5 clips, one speaker, so only a first look):** numbers and English words were often misheard, the fast WhatsApp clip was worst and also took about 16 seconds instead of about 3, and the 4-second pause in one clip did not confuse the model.
