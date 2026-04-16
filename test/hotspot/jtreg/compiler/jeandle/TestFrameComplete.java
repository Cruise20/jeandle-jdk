/*
 * Copyright (c) 2026, the Jeandle-JDK Authors. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

/*
 * @test TestFrameComplete.java
 * @summary Verify that Jeandle-compiled methods set Frame_Complete correctly
 *          so that AsyncGetCallTrace can walk the stack successfully.
 *
 *          This test replicates what async-profiler does: a JVMTI agent sends SIGPROF
 *          to a thread executing Jeandle-compiled code, and calls AsyncGetCallTrace
 *          from the signal handler with the interrupted thread's ucontext.
 *
 *          Without the Frame_Complete fix, safe_for_sender() fails for EVERY sample
 *          inside Jeandle-compiled code (is_frame_complete_at always returns false),
 *          causing AsyncGetCallTrace to return ticks_unknown_Java (-5) 100% of the time.
 *
 *          With the fix (Frame_Complete = _prolog_length), ASGCT succeeds for samples
 *          whose PC falls inside LLVM-generated code (after Frame_Complete). Samples
 *          landing in the Jeandle prolog (IC check, stack overflow check, etc.) still
 *          return unknown_Java, which is expected — the frame is genuinely incomplete
 *          at those PCs. The success rate depends on the ratio of prolog to body code.
 *
 * @requires os.family == "linux"
 * @requires vm.jvmti
 * @library /test/lib
 * @run main/othervm/native -agentlib:JeandleFrameCompleteTest
 *      -Xcomp -XX:-TieredCompilation -XX:+UseJeandleCompiler
 *      -XX:CompileCommand=compileonly,TestFrameComplete::*
 *      TestFrameComplete
 */

import jdk.test.lib.Asserts;

public class TestFrameComplete {

    static {
        System.loadLibrary("JeandleFrameCompleteTest");
    }

    private static volatile boolean stop = false;

    // Native methods implemented in libJeandleFrameCompleteTest.cpp.
    private static native void registerThread();
    private static native void runProfile(int numSamples, int intervalUs);
    private static native int getTotalSamples();
    private static native int getSuccessSamples();
    private static native int getUnknownJavaSamples();
    private static native int getOtherErrorSamples();

    // Without the fix, success = 0 (Frame_Complete is never set → is_frame_complete_at
    // always returns false → safe_for_sender always fails for compiled frames).
    // With the fix, success is typically 40-60% of total (samples in LLVM code body succeed;
    // samples in the Jeandle prolog before Frame_Complete still fail, which is expected).
    // We require at least 20% success to catch regressions with margin for noise.
    private static final int MIN_SUCCESS_PERCENT = 20;

    public static void main(String[] args) throws Exception {
        Thread worker = new Thread(TestFrameComplete::workerEntry);
        worker.setDaemon(true);
        worker.start();

        // Wait for the workload thread to enter Jeandle-compiled code.
        // With -Xcomp, methods are compiled on first invocation, so by the time
        // the loop body executes, it is already compiled.
        Thread.sleep(500);

        // Profile: send 2000 SIGPROF signals at 500us intervals (~1 second).
        runProfile(2000, 500);

        stop = true;
        worker.join(5000);

        int total = getTotalSamples();
        int success = getSuccessSamples();
        int unknownJava = getUnknownJavaSamples();
        int otherErrors = getOtherErrorSamples();

        System.out.println("ASGCT results: total=" + total +
                           " success=" + success +
                           " unknown_java=" + unknownJava +
                           " other_errors=" + otherErrors);

        Asserts.assertGreaterThan(total, 0,
            "No ASGCT samples collected — profiling did not work.");

        int minSuccess = total * MIN_SUCCESS_PERCENT / 100;
        Asserts.assertGreaterThanOrEqual(success, minSuccess,
            "Too few successful ASGCT samples (" + success + "/" + total + " = " +
            (success * 100 / total) + "%). " +
            "Without Frame_Complete fix, success would be 0. " +
            "Expected at least " + MIN_SUCCESS_PERCENT + "%.");
    }

    static void workerEntry() {
        registerThread();
        workload();
    }

    static void workload() {
        long sum = 0;
        int i = 0;
        while (!stop) {
            sum += fibonacci(15);
            sum += compute(i++);
        }
        if (sum == Long.MIN_VALUE) {
            System.out.println(sum);
        }
    }

    static int fibonacci(int n) {
        if (n <= 1) return n;
        return fibonacci(n - 1) + fibonacci(n - 2);
    }

    static long compute(int x) {
        long result = 0;
        for (int i = 0; i < 100; i++) {
            result += (x * i) ^ (x + i);
        }
        return result;
    }
}
