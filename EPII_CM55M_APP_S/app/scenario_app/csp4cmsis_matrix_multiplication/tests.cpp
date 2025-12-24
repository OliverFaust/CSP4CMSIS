#include "csp/csp4cmsis.h"
#include <cstdio>

using namespace csp;

// Constants for 3x3 Multiplication
#define DIM 3
#define TOTAL_PULSES (DIM + DIM - 1) // 3 + 3 - 1 = 5
#define TOTAL_PULSES DIM // Change from 5 to 3

// --- 1. Processing Element (PE) ---
class ProcessingElement : public CSProcess {
private:
    Chanin<int>  in_left, in_top;
    Chanout<int> out_right, out_bottom;
    int accumulator = 0;
    int row, col;

public:
    ProcessingElement(Chanin<int> l, Chanin<int> t, Chanout<int> r, Chanout<int> b, int r_idx, int c_idx)
        : in_left(l), in_top(t), out_right(r), out_bottom(b), row(r_idx), col(c_idx) {}

    void run() override {
        int valA, valB;
        
        for (int i = 0; i < TOTAL_PULSES; ++i) {
            // Trace: Waiting for data
            // printf("[PE %d,%d] Pulse %d: Waiting...\r\n", row, col, i);

            in_left >> valA;
            in_top >> valB;

            accumulator += (valA * valB);

            // Trace: Calculation pulse
            printf("[PE %d,%d] Pulse %d: In(%d, %d) Accum: %d\r\n", 
                    row, col, i, valA, valB, accumulator);

            out_right << valA;
            out_bottom << valB;
        }

        printf(">>> [PE %d,%d] COMPLETED. Final: %d\r\n", row, col, accumulator);
        
        while (true) vTaskDelay(portMAX_DELAY);
    }
};

// --- 2. Feeder Process ---
class Feeder : public CSProcess {
private:
    Chanout<int> out;
    int data[DIM];
    int stagger;
public:
    Feeder(Chanout<int> o, int d0, int d1, int d2, int s) 
        : out(o), stagger(s) {
        data[0] = d0; data[1] = d1; data[2] = d2;
    }

    void run() override {
        // 1. Staggering: inject 0s to delay the entry of real data
        for (int i = 0; i < stagger; ++i) out << 0;
        
        // 2. Real Data: inject the row/column values
        for (int i = 0; i < DIM; ++i) out << data[i];

        // 3. Flushing: inject 0s to keep the rest of the array moving
        int remaining = TOTAL_PULSES - DIM - stagger;
        for (int i = 0; i < remaining; ++i) out << 0;

        while (true) vTaskDelay(portMAX_DELAY);
    }
};

// --- 3. Sink Process ---
// Crucial: Sinks must consume data to allow the edge PEs to finish their 'out <<' calls.
class Sink : public CSProcess {
private:
    Chanin<int> in;
public:
    Sink(Chanin<int> i) : in(i) {}
    void run() override {
        int trash;
        for (int i = 0; i < TOTAL_PULSES; ++i) {
            in >> trash;
        }
        while (true) vTaskDelay(portMAX_DELAY);
    }
};

// --- 4. Main Application ---
void MainApp_Task(void* params) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    printf("\r\n--- Systolic Array 3x3: A * Identity ---\r\n");

    // Static channels for the grid
    static Channel<int> h[3][4]; // Horizontal channels
    static Channel<int> v[4][3]; // Vertical channels

    // Matrix A (Rows) - Stagger 0
    static Feeder row0(h[0][0].writer(), 1, 2, 3, 0); 
    static Feeder row1(h[1][0].writer(), 4, 5, 6, 0); 
    static Feeder row2(h[2][0].writer(), 7, 8, 9, 0); 

    // Matrix B (Cols) - Identity - Stagger 0
    static Feeder col0(v[0][0].writer(), 1, 0, 0, 0); 
    static Feeder col1(v[0][1].writer(), 0, 1, 0, 0); 
    static Feeder col2(v[0][2].writer(), 0, 0, 1, 0);

    // Grid of PEs
    static ProcessingElement pe00(h[0][0].reader(), v[0][0].reader(), h[0][1].writer(), v[1][0].writer(), 0, 0);
    static ProcessingElement pe01(h[0][1].reader(), v[0][1].reader(), h[0][2].writer(), v[1][1].writer(), 0, 1);
    static ProcessingElement pe02(h[0][2].reader(), v[0][2].reader(), h[0][3].writer(), v[1][2].writer(), 0, 2);

    static ProcessingElement pe10(h[1][0].reader(), v[1][0].reader(), h[1][1].writer(), v[2][0].writer(), 1, 0);
    static ProcessingElement pe11(h[1][1].reader(), v[1][1].reader(), h[1][2].writer(), v[2][1].writer(), 1, 1);
    static ProcessingElement pe12(h[1][2].reader(), v[1][2].reader(), h[1][3].writer(), v[2][2].writer(), 1, 2);

    static ProcessingElement pe20(h[2][0].reader(), v[2][0].reader(), h[2][1].writer(), v[3][0].writer(), 2, 0);
    static ProcessingElement pe21(h[2][1].reader(), v[2][1].reader(), h[2][2].writer(), v[3][1].writer(), 2, 1);
    static ProcessingElement pe22(h[2][2].reader(), v[2][2].reader(), h[2][3].writer(), v[3][2].writer(), 2, 2);

    // Edge Sinks
    static Sink sH0(h[0][3].reader()), sH1(h[1][3].reader()), sH2(h[2][3].reader());
    static Sink sV0(v[3][0].reader()), sV1(v[3][1].reader()), sV2(v[3][2].reader());

    Run(
        InParallel(
            row0, row1, row2, 
            col0, col1, col2, 
            pe00, pe01, pe02, 
            pe10, pe11, pe12, 
            pe20, pe21, pe22,
            sH0, sH1, sH2, sV0, sV1, sV2
        ),
        ExecutionMode::StaticNetwork
    );
}

void RunProcessingChainTest(void) {
    // Note: Task creation is the only 'dynamic' part remaining, standard for FreeRTOS
    xTaskCreate(MainApp_Task, "MainApp", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
}
