#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#include <Arduino.h>

template <typename T, size_t Size>
class CircularBuffer {
public:
    CircularBuffer() : head(0), count(0) {
        memset(buffer, 0, sizeof(buffer));
    }

    void push(T value) {
        buffer[head] = value;
        head = (head + 1) % Size;
        if (count < Size) count++;
    }

    T sum() const {
        T total = 0;
        for (size_t i = 0; i < count; i++) {
            total += buffer[i];
        }
        return total;
    }
    
    // Add value to the current head without advancing (for accumulating partials)
    // Actually, for this specific use case, we might want to just overwrite or add.
    // The requirement is "fed from left to right". 
    // Minute buffer: Push 1-min total.
    
    void clear() {
        head = 0;
        count = 0;
        memset(buffer, 0, sizeof(buffer));
    }
    
    // Fill buffer with value (useful for restoring from persistence if simplified)
    void fill(T* values, size_t amount) {
        clear();
        for(size_t i=0; i<amount && i<Size; i++) {
            push(values[i]);
        }
    }
    
    // Access raw buffer for NVS/RTC persistence
    const T* getBuffer() const { return buffer; }
    size_t getHead() const { return head; }
    size_t getCount() const { return count; }
    
    // Restore state
    void restore(const T* values, size_t storedHead, size_t storedCount) {
        if (storedCount > Size) storedCount = Size;
        memcpy(buffer, values, sizeof(T) * Size); // Copy full buffer just in case
        head = storedHead % Size;
        count = storedCount;
    }

private:
    T buffer[Size];
    size_t head;
    size_t count;
};

#endif
