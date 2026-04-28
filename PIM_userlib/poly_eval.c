/*
 * TODO
 * Rewrite following function into benchmark style
 */
#define ARRAY_SIZE 1000000 // Tune for cache size

void poly_evaluate(float X[ARRAY_SIZE], float Y[ARRAY_SIZE]) {
    // Coefficients for a random 5th degree polynomial
    float c5 = 2.5f, c4 = -1.2f, c3 = 3.4f, c2 = -0.5f, c1 = 1.1f, c0 = 4.0f;

    for (int i = 0; i < ARRAY_SIZE; i++) {
        float x = X[i];
        
        // Horner's method for calculating: c5*x^5 + c4*x^4 + c3*x^3 + c2*x^2 + c1*x + c0
        // This creates a tight dependency chain of Multiply-Accumulate (MAC) operations
        float result = ((((c5 * x + c4) * x + c3) * x + c2) * x + c1) * x + c0;
        
        Y[i] = result;
    }
}
