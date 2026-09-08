#ifndef DES_H
#define DES_H

// encData 输出: 明文每4字节 → 16个十六进制字符(4倍长度)
// encSize 为 encData 缓冲区总容量(含结尾 '\0'), 超出即截断
void strEnc(const char* data, const char* firstKey,
            const char* secondKey, const char* thirdKey,
            char* encData, unsigned int encSize);

#endif