/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
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
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/assembler.hpp"
#include "asm/assembler.inline.hpp"
#include "oops/methodData.hpp"
#include "opto/c2_MacroAssembler.hpp"
#include "opto/intrinsicnode.hpp"
#include "opto/opcodes.hpp"
#include "runtime/biasedLocking.hpp"
#include "runtime/objectMonitor.hpp"
#include "runtime/stubRoutines.hpp"

void C2_MacroAssembler::setvectmask(Register dst, Register src) {
  guarantee(PostLoopMultiversioning, "must be");
  Assembler::movl(dst, 1);
  Assembler::shlxl(dst, dst, src);
  Assembler::decl(dst);
  Assembler::kmovdl(k1, dst);
  Assembler::movl(dst, src);
}

void C2_MacroAssembler::restorevectmask() {
  guarantee(PostLoopMultiversioning, "must be");
  Assembler::knotwl(k1, k0);
}

#if INCLUDE_RTM_OPT

// Update rtm_counters based on abort status
// input: abort_status
//        rtm_counters (RTMLockingCounters*)
// flags are killed
void C2_MacroAssembler::rtm_counters_update(Register abort_status, Register rtm_counters) {

  atomic_incptr(Address(rtm_counters, RTMLockingCounters::abort_count_offset()));
  if (PrintPreciseRTMLockingStatistics) {
    for (int i = 0; i < RTMLockingCounters::ABORT_STATUS_LIMIT; i++) {
      Label check_abort;
      testl(abort_status, (1<<i));
      jccb(Assembler::equal, check_abort);
      atomic_incptr(Address(rtm_counters, RTMLockingCounters::abortX_count_offset() + (i * sizeof(uintx))));
      bind(check_abort);
    }
  }
}

// Branch if (random & (count-1) != 0), count is 2^n
// tmp, scr and flags are killed
void C2_MacroAssembler::branch_on_random_using_rdtsc(Register tmp, Register scr, int count, Label& brLabel) {
  assert(tmp == rax, "");
  assert(scr == rdx, "");
  rdtsc(); // modifies EDX:EAX
  andptr(tmp, count-1);
  jccb(Assembler::notZero, brLabel);
}

// Perform abort ratio calculation, set no_rtm bit if high ratio
// input:  rtm_counters_Reg (RTMLockingCounters* address)
// tmpReg, rtm_counters_Reg and flags are killed
void C2_MacroAssembler::rtm_abort_ratio_calculation(Register tmpReg,
                                                    Register rtm_counters_Reg,
                                                    RTMLockingCounters* rtm_counters,
                                                    Metadata* method_data) {
  Label L_done, L_check_always_rtm1, L_check_always_rtm2;

  if (RTMLockingCalculationDelay > 0) {
    // Delay calculation
    movptr(tmpReg, ExternalAddress((address) RTMLockingCounters::rtm_calculation_flag_addr()), tmpReg);
    testptr(tmpReg, tmpReg);
    jccb(Assembler::equal, L_done);
  }
  // Abort ratio calculation only if abort_count > RTMAbortThreshold
  //   Aborted transactions = abort_count * 100
  //   All transactions = total_count *  RTMTotalCountIncrRate
  //   Set no_rtm bit if (Aborted transactions >= All transactions * RTMAbortRatio)

  movptr(tmpReg, Address(rtm_counters_Reg, RTMLockingCounters::abort_count_offset()));
  cmpptr(tmpReg, RTMAbortThreshold);
  jccb(Assembler::below, L_check_always_rtm2);
  imulptr(tmpReg, tmpReg, 100);

  Register scrReg = rtm_counters_Reg;
  movptr(scrReg, Address(rtm_counters_Reg, RTMLockingCounters::total_count_offset()));
  imulptr(scrReg, scrReg, RTMTotalCountIncrRate);
  imulptr(scrReg, scrReg, RTMAbortRatio);
  cmpptr(tmpReg, scrReg);
  jccb(Assembler::below, L_check_always_rtm1);
  if (method_data != NULL) {
    // set rtm_state to "no rtm" in MDO
    mov_metadata(tmpReg, method_data);
    lock();
    orl(Address(tmpReg, MethodData::rtm_state_offset_in_bytes()), NoRTM);
  }
  jmpb(L_done);
  bind(L_check_always_rtm1);
  // Reload RTMLockingCounters* address
  lea(rtm_counters_Reg, ExternalAddress((address)rtm_counters));
  bind(L_check_always_rtm2);
  movptr(tmpReg, Address(rtm_counters_Reg, RTMLockingCounters::total_count_offset()));
  cmpptr(tmpReg, RTMLockingThreshold / RTMTotalCountIncrRate);
  jccb(Assembler::below, L_done);
  if (method_data != NULL) {
    // set rtm_state to "always rtm" in MDO
    mov_metadata(tmpReg, method_data);
    lock();
    orl(Address(tmpReg, MethodData::rtm_state_offset_in_bytes()), UseRTM);
  }
  bind(L_done);
}

// Update counters and perform abort ratio calculation
// input:  abort_status_Reg
// rtm_counters_Reg, flags are killed
void C2_MacroAssembler::rtm_profiling(Register abort_status_Reg,
                                      Register rtm_counters_Reg,
                                      RTMLockingCounters* rtm_counters,
                                      Metadata* method_data,
                                      bool profile_rtm) {

  assert(rtm_counters != NULL, "should not be NULL when profiling RTM");
  // update rtm counters based on rax value at abort
  // reads abort_status_Reg, updates flags
  lea(rtm_counters_Reg, ExternalAddress((address)rtm_counters));
  rtm_counters_update(abort_status_Reg, rtm_counters_Reg);
  if (profile_rtm) {
    // Save abort status because abort_status_Reg is used by following code.
    if (RTMRetryCount > 0) {
      push(abort_status_Reg);
    }
    assert(rtm_counters != NULL, "should not be NULL when profiling RTM");
    rtm_abort_ratio_calculation(abort_status_Reg, rtm_counters_Reg, rtm_counters, method_data);
    // restore abort status
    if (RTMRetryCount > 0) {
      pop(abort_status_Reg);
    }
  }
}

// Retry on abort if abort's status is 0x6: can retry (0x2) | memory conflict (0x4)
// inputs: retry_count_Reg
//       : abort_status_Reg
// output: retry_count_Reg decremented by 1
// flags are killed
void C2_MacroAssembler::rtm_retry_lock_on_abort(Register retry_count_Reg, Register abort_status_Reg, Label& retryLabel) {
  Label doneRetry;
  assert(abort_status_Reg == rax, "");
  // The abort reason bits are in eax (see all states in rtmLocking.hpp)
  // 0x6 = conflict on which we can retry (0x2) | memory conflict (0x4)
  // if reason is in 0x6 and retry count != 0 then retry
  andptr(abort_status_Reg, 0x6);
  jccb(Assembler::zero, doneRetry);
  testl(retry_count_Reg, retry_count_Reg);
  jccb(Assembler::zero, doneRetry);
  pause();
  decrementl(retry_count_Reg);
  jmp(retryLabel);
  bind(doneRetry);
}

// Spin and retry if lock is busy,
// inputs: box_Reg (monitor address)
//       : retry_count_Reg
// output: retry_count_Reg decremented by 1
//       : clear z flag if retry count exceeded
// tmp_Reg, scr_Reg, flags are killed
void C2_MacroAssembler::rtm_retry_lock_on_busy(Register retry_count_Reg, Register box_Reg,
                                               Register tmp_Reg, Register scr_Reg, Label& retryLabel) {
  Label SpinLoop, SpinExit, doneRetry;
  int owner_offset = OM_OFFSET_NO_MONITOR_VALUE_TAG(owner);

  testl(retry_count_Reg, retry_count_Reg);
  jccb(Assembler::zero, doneRetry);
  decrementl(retry_count_Reg);
  movptr(scr_Reg, RTMSpinLoopCount);

  bind(SpinLoop);
  pause();
  decrementl(scr_Reg);
  jccb(Assembler::lessEqual, SpinExit);
  movptr(tmp_Reg, Address(box_Reg, owner_offset));
  testptr(tmp_Reg, tmp_Reg);
  jccb(Assembler::notZero, SpinLoop);

  bind(SpinExit);
  jmp(retryLabel);
  bind(doneRetry);
  incrementl(retry_count_Reg); // clear z flag
}

// Use RTM for normal stack locks
// Input: objReg (object to lock)
void C2_MacroAssembler::rtm_stack_locking(Register objReg, Register tmpReg, Register scrReg,
                                         Register retry_on_abort_count_Reg,
                                         RTMLockingCounters* stack_rtm_counters,
                                         Metadata* method_data, bool profile_rtm,
                                         Label& DONE_LABEL, Label& IsInflated) {
  assert(UseRTMForStackLocks, "why call this otherwise?");
  assert(!UseBiasedLocking, "Biased locking is not supported with RTM locking");
  assert(tmpReg == rax, "");
  assert(scrReg == rdx, "");
  Label L_rtm_retry, L_decrement_retry, L_on_abort;

  if (RTMRetryCount > 0) {
    movl(retry_on_abort_count_Reg, RTMRetryCount); // Retry on abort
    bind(L_rtm_retry);
  }
  movptr(tmpReg, Address(objReg, oopDesc::mark_offset_in_bytes()));
  testptr(tmpReg, markWord::monitor_value);  // inflated vs stack-locked|neutral|biased
  jcc(Assembler::notZero, IsInflated);

  if (PrintPreciseRTMLockingStatistics || profile_rtm) {
    Label L_noincrement;
    if (RTMTotalCountIncrRate > 1) {
      // tmpReg, scrReg and flags are killed
      branch_on_random_using_rdtsc(tmpReg, scrReg, RTMTotalCountIncrRate, L_noincrement);
    }
    assert(stack_rtm_counters != NULL, "should not be NULL when profiling RTM");
    atomic_incptr(ExternalAddress((address)stack_rtm_counters->total_count_addr()), scrReg);
    bind(L_noincrement);
  }
  xbegin(L_on_abort);
  movptr(tmpReg, Address(objReg, oopDesc::mark_offset_in_bytes()));       // fetch markword
  andptr(tmpReg, markWord::biased_lock_mask_in_place); // look at 3 lock bits
  cmpptr(tmpReg, markWord::unlocked_value);            // bits = 001 unlocked
  jcc(Assembler::equal, DONE_LABEL);        // all done if unlocked

  Register abort_status_Reg = tmpReg; // status of abort is stored in RAX
  if (UseRTMXendForLockBusy) {
    xend();
    movptr(abort_status_Reg, 0x2);   // Set the abort status to 2 (so we can retry)
    jmp(L_decrement_retry);
  }
  else {
    xabort(0);
  }
  bind(L_on_abort);
  if (PrintPreciseRTMLockingStatistics || profile_rtm) {
    rtm_profiling(abort_status_Reg, scrReg, stack_rtm_counters, method_data, profile_rtm);
  }
  bind(L_decrement_retry);
  if (RTMRetryCount > 0) {
    // retry on lock abort if abort status is 'can retry' (0x2) or 'memory conflict' (0x4)
    rtm_retry_lock_on_abort(retry_on_abort_count_Reg, abort_status_Reg, L_rtm_retry);
  }
}

// Use RTM for inflating locks
// inputs: objReg (object to lock)
//         boxReg (on-stack box address (displaced header location) - KILLED)
//         tmpReg (ObjectMonitor address + markWord::monitor_value)
void C2_MacroAssembler::rtm_inflated_locking(Register objReg, Register boxReg, Register tmpReg,
                                            Register scrReg, Register retry_on_busy_count_Reg,
                                            Register retry_on_abort_count_Reg,
                                            RTMLockingCounters* rtm_counters,
                                            Metadata* method_data, bool profile_rtm,
                                            Label& DONE_LABEL) {
  assert(UseRTMLocking, "why call this otherwise?");
  assert(tmpReg == rax, "");
  assert(scrReg == rdx, "");
  Label L_rtm_retry, L_decrement_retry, L_on_abort;
  int owner_offset = OM_OFFSET_NO_MONITOR_VALUE_TAG(owner);

  // Without cast to int32_t this style of movptr will destroy r10 which is typically obj.
  movptr(Address(boxReg, 0), (int32_t)intptr_t(markWord::unused_mark().value()));
  movptr(boxReg, tmpReg); // Save ObjectMonitor address

  if (RTMRetryCount > 0) {
    movl(retry_on_busy_count_Reg, RTMRetryCount);  // Retry on lock busy
    movl(retry_on_abort_count_Reg, RTMRetryCount); // Retry on abort
    bind(L_rtm_retry);
  }
  if (PrintPreciseRTMLockingStatistics || profile_rtm) {
    Label L_noincrement;
    if (RTMTotalCountIncrRate > 1) {
      // tmpReg, scrReg and flags are killed
      branch_on_random_using_rdtsc(tmpReg, scrReg, RTMTotalCountIncrRate, L_noincrement);
    }
    assert(rtm_counters != NULL, "should not be NULL when profiling RTM");
    atomic_incptr(ExternalAddress((address)rtm_counters->total_count_addr()), scrReg);
    bind(L_noincrement);
  }
  xbegin(L_on_abort);
  movptr(tmpReg, Address(objReg, oopDesc::mark_offset_in_bytes()));
  movptr(tmpReg, Address(tmpReg, owner_offset));
  testptr(tmpReg, tmpReg);
  jcc(Assembler::zero, DONE_LABEL);
  if (UseRTMXendForLockBusy) {
    xend();
    jmp(L_decrement_retry);
  }
  else {
    xabort(0);
  }
  bind(L_on_abort);
  Register abort_status_Reg = tmpReg; // status of abort is stored in RAX
  if (PrintPreciseRTMLockingStatistics || profile_rtm) {
    rtm_profiling(abort_status_Reg, scrReg, rtm_counters, method_data, profile_rtm);
  }
  if (RTMRetryCount > 0) {
    // retry on lock abort if abort status is 'can retry' (0x2) or 'memory conflict' (0x4)
    rtm_retry_lock_on_abort(retry_on_abort_count_Reg, abort_status_Reg, L_rtm_retry);
  }

  movptr(tmpReg, Address(boxReg, owner_offset)) ;
  testptr(tmpReg, tmpReg) ;
  jccb(Assembler::notZero, L_decrement_retry) ;

  // Appears unlocked - try to swing _owner from null to non-null.
  // Invariant: tmpReg == 0.  tmpReg is EAX which is the implicit cmpxchg comparand.
#ifdef _LP64
  Register threadReg = r15_thread;
#else
  get_thread(scrReg);
  Register threadReg = scrReg;
#endif
  lock();
  cmpxchgptr(threadReg, Address(boxReg, owner_offset)); // Updates tmpReg

  if (RTMRetryCount > 0) {
    // success done else retry
    jccb(Assembler::equal, DONE_LABEL) ;
    bind(L_decrement_retry);
    // Spin and retry if lock is busy.
    rtm_retry_lock_on_busy(retry_on_busy_count_Reg, boxReg, tmpReg, scrReg, L_rtm_retry);
  }
  else {
    bind(L_decrement_retry);
  }
}

#endif //  INCLUDE_RTM_OPT

// fast_lock and fast_unlock used by C2

// Because the transitions from emitted code to the runtime
// monitorenter/exit helper stubs are so slow it's critical that
// we inline both the stack-locking fast path and the inflated fast path.
//
// See also: cmpFastLock and cmpFastUnlock.
//
// What follows is a specialized inline transliteration of the code
// in enter() and exit(). If we're concerned about I$ bloat another
// option would be to emit TrySlowEnter and TrySlowExit methods
// at startup-time.  These methods would accept arguments as
// (rax,=Obj, rbx=Self, rcx=box, rdx=Scratch) and return success-failure
// indications in the icc.ZFlag.  fast_lock and fast_unlock would simply
// marshal the arguments and emit calls to TrySlowEnter and TrySlowExit.
// In practice, however, the # of lock sites is bounded and is usually small.
// Besides the call overhead, TrySlowEnter and TrySlowExit might suffer
// if the processor uses simple bimodal branch predictors keyed by EIP
// Since the helper routines would be called from multiple synchronization
// sites.
//
// An even better approach would be write "MonitorEnter()" and "MonitorExit()"
// in java - using j.u.c and unsafe - and just bind the lock and unlock sites
// to those specialized methods.  That'd give us a mostly platform-independent
// implementation that the JITs could optimize and inline at their pleasure.
// Done correctly, the only time we'd need to cross to native could would be
// to park() or unpark() threads.  We'd also need a few more unsafe operators
// to (a) prevent compiler-JIT reordering of non-volatile accesses, and
// (b) explicit barriers or fence operations.
//
// TODO:
//
// *  Arrange for C2 to pass "Self" into fast_lock and fast_unlock in one of the registers (scr).
//    This avoids manifesting the Self pointer in the fast_lock and fast_unlock terminals.
//    Given TLAB allocation, Self is usually manifested in a register, so passing it into
//    the lock operators would typically be faster than reifying Self.
//
// *  Ideally I'd define the primitives as:
//       fast_lock   (nax Obj, nax box, EAX tmp, nax scr) where box, tmp and scr are KILLED.
//       fast_unlock (nax Obj, EAX box, nax tmp) where box and tmp are KILLED
//    Unfortunately ADLC bugs prevent us from expressing the ideal form.
//    Instead, we're stuck with a rather awkward and brittle register assignments below.
//    Furthermore the register assignments are overconstrained, possibly resulting in
//    sub-optimal code near the synchronization site.
//
// *  Eliminate the sp-proximity tests and just use "== Self" tests instead.
//    Alternately, use a better sp-proximity test.
//
// *  Currently ObjectMonitor._Owner can hold either an sp value or a (THREAD *) value.
//    Either one is sufficient to uniquely identify a thread.
//    TODO: eliminate use of sp in _owner and use get_thread(tr) instead.
//
// *  Intrinsify notify() and notifyAll() for the common cases where the
//    object is locked by the calling thread but the waitlist is empty.
//    avoid the expensive JNI call to JVM_Notify() and JVM_NotifyAll().
//
// *  use jccb and jmpb instead of jcc and jmp to improve code density.
//    But beware of excessive branch density on AMD Opterons.
//
// *  Both fast_lock and fast_unlock set the ICC.ZF to indicate success
//    or failure of the fast path.  If the fast path fails then we pass
//    control to the slow path, typically in C.  In fast_lock and
//    fast_unlock we often branch to DONE_LABEL, just to find that C2
//    will emit a conditional branch immediately after the node.
//    So we have branches to branches and lots of ICC.ZF games.
//    Instead, it might be better to have C2 pass a "FailureLabel"
//    into fast_lock and fast_unlock.  In the case of success, control
//    will drop through the node.  ICC.ZF is undefined at exit.
//    In the case of failure, the node will branch directly to the
//    FailureLabel


// obj: object to lock
// box: on-stack box address (displaced header location) - KILLED
// rax,: tmp -- KILLED
// scr: tmp -- KILLED
void C2_MacroAssembler::fast_lock(Register objReg, Register boxReg, Register tmpReg,
                                 Register scrReg, Register cx1Reg, Register cx2Reg,
                                 BiasedLockingCounters* counters,
                                 RTMLockingCounters* rtm_counters,
                                 RTMLockingCounters* stack_rtm_counters,
                                 Metadata* method_data,
                                 bool use_rtm, bool profile_rtm) {
  // Ensure the register assignments are disjoint
  assert(tmpReg == rax, "");

  if (use_rtm) {
    assert_different_registers(objReg, boxReg, tmpReg, scrReg, cx1Reg, cx2Reg);
  } else {
    assert(cx2Reg == noreg, "");
    assert_different_registers(objReg, boxReg, tmpReg, scrReg);
  }

  if (counters != NULL) {
    atomic_incl(ExternalAddress((address)counters->total_entry_count_addr()), scrReg);
  }

  // Possible cases that we'll encounter in fast_lock
  // ------------------------------------------------
  // * Inflated
  //    -- unlocked
  //    -- Locked
  //       = by self
  //       = by other
  // * biased
  //    -- by Self
  //    -- by other
  // * neutral
  // * stack-locked
  //    -- by self
  //       = sp-proximity test hits
  //       = sp-proximity test generates false-negative
  //    -- by other
  //

  Label IsInflated, DONE_LABEL;

  // it's stack-locked, biased or neutral
  // TODO: optimize away redundant LDs of obj->mark and improve the markword triage
  // order to reduce the number of conditional branches in the most common cases.
  // Beware -- there's a subtle invariant that fetch of the markword
  // at [FETCH], below, will never observe a biased encoding (*101b).
  // If this invariant is not held we risk exclusion (safety) failure.
  if (UseBiasedLocking && !UseOptoBiasInlining) {
    biased_locking_enter(boxReg, objReg, tmpReg, scrReg, cx1Reg, false, DONE_LABEL, NULL, counters);
  }

#if INCLUDE_RTM_OPT
  if (UseRTMForStackLocks && use_rtm) {
    rtm_stack_locking(objReg, tmpReg, scrReg, cx2Reg,
                      stack_rtm_counters, method_data, profile_rtm,
                      DONE_LABEL, IsInflated);
  }
#endif // INCLUDE_RTM_OPT

  movptr(tmpReg, Address(objReg, oopDesc::mark_offset_in_bytes()));          // [FETCH]
  testptr(tmpReg, markWord::monitor_value); // inflated vs stack-locked|neutral|biased
  jccb(Assembler::notZero, IsInflated);

  // Attempt stack-locking ...
  orptr (tmpReg, markWord::unlocked_value);
  movptr(Address(boxReg, 0), tmpReg);          // Anticipate successful CAS
  lock();
  cmpxchgptr(boxReg, Address(objReg, oopDesc::mark_offset_in_bytes()));      // Updates tmpReg
  if (counters != NULL) {
    cond_inc32(Assembler::equal,
               ExternalAddress((address)counters->fast_path_entry_count_addr()));
  }
  jcc(Assembler::equal, DONE_LABEL);           // Success

  // Recursive locking.
  // The object is stack-locked: markword contains stack pointer to BasicLock.
  // Locked by current thread if difference with current SP is less than one page.
  subptr(tmpReg, rsp);
  // Next instruction set ZFlag == 1 (Success) if difference is less then one page.
  andptr(tmpReg, (int32_t) (NOT_LP64(0xFFFFF003) LP64_ONLY(7 - os::vm_page_size())) );
  movptr(Address(boxReg, 0), tmpReg);
  if (counters != NULL) {
    cond_inc32(Assembler::equal,
               ExternalAddress((address)counters->fast_path_entry_count_addr()));
  }
  jmp(DONE_LABEL);

  bind(IsInflated);
  // The object is inflated. tmpReg contains pointer to ObjectMonitor* + markWord::monitor_value

#if INCLUDE_RTM_OPT
  // Use the same RTM locking code in 32- and 64-bit VM.
  if (use_rtm) {
    rtm_inflated_locking(objReg, boxReg, tmpReg, scrReg, cx1Reg, cx2Reg,
                         rtm_counters, method_data, profile_rtm, DONE_LABEL);
  } else {
#endif // INCLUDE_RTM_OPT

#ifndef _LP64
  // The object is inflated.

  // boxReg refers to the on-stack BasicLock in the current frame.
  // We'd like to write:
  //   set box->_displaced_header = markWord::unused_mark().  Any non-0 value suffices.
  // This is convenient but results a ST-before-CAS penalty.  The following CAS suffers
  // additional latency as we have another ST in the store buffer that must drain.

  // avoid ST-before-CAS
  // register juggle because we need tmpReg for cmpxchgptr below
  movptr(scrReg, boxReg);
  movptr(boxReg, tmpReg);                   // consider: LEA box, [tmp-2]

  // Optimistic form: consider XORL tmpReg,tmpReg
  movptr(tmpReg, NULL_WORD);

  // Appears unlocked - try to swing _owner from null to non-null.
  // Ideally, I'd manifest "Self" with get_thread and then attempt
  // to CAS the register containing Self into m->Owner.
  // But we don't have enough registers, so instead we can either try to CAS
  // rsp or the address of the box (in scr) into &m->owner.  If the CAS succeeds
  // we later store "Self" into m->Owner.  Transiently storing a stack address
  // (rsp or the address of the box) into  m->owner is harmless.
  // Invariant: tmpReg == 0.  tmpReg is EAX which is the implicit cmpxchg comparand.
  lock();
  cmpxchgptr(scrReg, Address(boxReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)));
  movptr(Address(scrReg, 0), 3);          // box->_displaced_header = 3
  // If we weren't able to swing _owner from NULL to the BasicLock
  // then take the slow path.
  jccb  (Assembler::notZero, DONE_LABEL);
  // update _owner from BasicLock to thread
  get_thread (scrReg);                    // beware: clobbers ICCs
  movptr(Address(boxReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)), scrReg);
  xorptr(boxReg, boxReg);                 // set icc.ZFlag = 1 to indicate success

  // If the CAS fails we can either retry or pass control to the slow path.
  // We use the latter tactic.
  // Pass the CAS result in the icc.ZFlag into DONE_LABEL
  // If the CAS was successful ...
  //   Self has acquired the lock
  //   Invariant: m->_recursions should already be 0, so we don't need to explicitly set it.
  // Intentional fall-through into DONE_LABEL ...
#else // _LP64
  // It's inflated and we use scrReg for ObjectMonitor* in this section.
  movq(scrReg, tmpReg);
  xorq(tmpReg, tmpReg);
  lock();
  cmpxchgptr(r15_thread, Address(scrReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)));
  // Unconditionally set box->_displaced_header = markWord::unused_mark().
  // Without cast to int32_t this style of movptr will destroy r10 which is typically obj.
  movptr(Address(boxReg, 0), (int32_t)intptr_t(markWord::unused_mark().value()));
  // Intentional fall-through into DONE_LABEL ...
  // Propagate ICC.ZF from CAS above into DONE_LABEL.
#endif // _LP64
#if INCLUDE_RTM_OPT
  } // use_rtm()
#endif
  // DONE_LABEL is a hot target - we'd really like to place it at the
  // start of cache line by padding with NOPs.
  // See the AMD and Intel software optimization manuals for the
  // most efficient "long" NOP encodings.
  // Unfortunately none of our alignment mechanisms suffice.
  bind(DONE_LABEL);

  // At DONE_LABEL the icc ZFlag is set as follows ...
  // fast_unlock uses the same protocol.
  // ZFlag == 1 -> Success
  // ZFlag == 0 -> Failure - force control through the slow path
}

// obj: object to unlock
// box: box address (displaced header location), killed.  Must be EAX.
// tmp: killed, cannot be obj nor box.
//
// Some commentary on balanced locking:
//
// fast_lock and fast_unlock are emitted only for provably balanced lock sites.
// Methods that don't have provably balanced locking are forced to run in the
// interpreter - such methods won't be compiled to use fast_lock and fast_unlock.
// The interpreter provides two properties:
// I1:  At return-time the interpreter automatically and quietly unlocks any
//      objects acquired the current activation (frame).  Recall that the
//      interpreter maintains an on-stack list of locks currently held by
//      a frame.
// I2:  If a method attempts to unlock an object that is not held by the
//      the frame the interpreter throws IMSX.
//
// Lets say A(), which has provably balanced locking, acquires O and then calls B().
// B() doesn't have provably balanced locking so it runs in the interpreter.
// Control returns to A() and A() unlocks O.  By I1 and I2, above, we know that O
// is still locked by A().
//
// The only other source of unbalanced locking would be JNI.  The "Java Native Interface:
// Programmer's Guide and Specification" claims that an object locked by jni_monitorenter
// should not be unlocked by "normal" java-level locking and vice-versa.  The specification
// doesn't specify what will occur if a program engages in such mixed-mode locking, however.
// Arguably given that the spec legislates the JNI case as undefined our implementation
// could reasonably *avoid* checking owner in fast_unlock().
// In the interest of performance we elide m->Owner==Self check in unlock.
// A perfectly viable alternative is to elide the owner check except when
// Xcheck:jni is enabled.

void C2_MacroAssembler::fast_unlock(Register objReg, Register boxReg, Register tmpReg, bool use_rtm) {
  assert(boxReg == rax, "");
  assert_different_registers(objReg, boxReg, tmpReg);

  Label DONE_LABEL, Stacked, CheckSucc;

  // Critically, the biased locking test must have precedence over
  // and appear before the (box->dhw == 0) recursive stack-lock test.
  if (UseBiasedLocking && !UseOptoBiasInlining) {
    biased_locking_exit(objReg, tmpReg, DONE_LABEL);
  }

#if INCLUDE_RTM_OPT
  if (UseRTMForStackLocks && use_rtm) {
    assert(!UseBiasedLocking, "Biased locking is not supported with RTM locking");
    Label L_regular_unlock;
    movptr(tmpReg, Address(objReg, oopDesc::mark_offset_in_bytes())); // fetch markword
    andptr(tmpReg, markWord::biased_lock_mask_in_place);              // look at 3 lock bits
    cmpptr(tmpReg, markWord::unlocked_value);                         // bits = 001 unlocked
    jccb(Assembler::notEqual, L_regular_unlock);                      // if !HLE RegularLock
    xend();                                                           // otherwise end...
    jmp(DONE_LABEL);                                                  // ... and we're done
    bind(L_regular_unlock);
  }
#endif

  cmpptr(Address(boxReg, 0), (int32_t)NULL_WORD);                   // Examine the displaced header
  jcc   (Assembler::zero, DONE_LABEL);                              // 0 indicates recursive stack-lock
  movptr(tmpReg, Address(objReg, oopDesc::mark_offset_in_bytes())); // Examine the object's markword
  testptr(tmpReg, markWord::monitor_value);                         // Inflated?
  jccb  (Assembler::zero, Stacked);

  // It's inflated.
#if INCLUDE_RTM_OPT
  if (use_rtm) {
    Label L_regular_inflated_unlock;
    int owner_offset = OM_OFFSET_NO_MONITOR_VALUE_TAG(owner);
    movptr(boxReg, Address(tmpReg, owner_offset));
    testptr(boxReg, boxReg);
    jccb(Assembler::notZero, L_regular_inflated_unlock);
    xend();
    jmpb(DONE_LABEL);
    bind(L_regular_inflated_unlock);
  }
#endif

  // Despite our balanced locking property we still check that m->_owner == Self
  // as java routines or native JNI code called by this thread might
  // have released the lock.
  // Refer to the comments in synchronizer.cpp for how we might encode extra
  // state in _succ so we can avoid fetching EntryList|cxq.
  //
  // I'd like to add more cases in fast_lock() and fast_unlock() --
  // such as recursive enter and exit -- but we have to be wary of
  // I$ bloat, T$ effects and BP$ effects.
  //
  // If there's no contention try a 1-0 exit.  That is, exit without
  // a costly MEMBAR or CAS.  See synchronizer.cpp for details on how
  // we detect and recover from the race that the 1-0 exit admits.
  //
  // Conceptually fast_unlock() must execute a STST|LDST "release" barrier
  // before it STs null into _owner, releasing the lock.  Updates
  // to data protected by the critical section must be visible before
  // we drop the lock (and thus before any other thread could acquire
  // the lock and observe the fields protected by the lock).
  // IA32's memory-model is SPO, so STs are ordered with respect to
  // each other and there's no need for an explicit barrier (fence).
  // See also http://gee.cs.oswego.edu/dl/jmm/cookbook.html.
#ifndef _LP64
  get_thread (boxReg);

  // Note that we could employ various encoding schemes to reduce
  // the number of loads below (currently 4) to just 2 or 3.
  // Refer to the comments in synchronizer.cpp.
  // In practice the chain of fetches doesn't seem to impact performance, however.
  xorptr(boxReg, boxReg);
  orptr(boxReg, Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(recursions)));
  jccb  (Assembler::notZero, DONE_LABEL);
  movptr(boxReg, Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(EntryList)));
  orptr(boxReg, Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(cxq)));
  jccb  (Assembler::notZero, CheckSucc);
  movptr(Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)), NULL_WORD);
  jmpb  (DONE_LABEL);

  bind (Stacked);
  // It's not inflated and it's not recursively stack-locked and it's not biased.
  // It must be stack-locked.
  // Try to reset the header to displaced header.
  // The "box" value on the stack is stable, so we can reload
  // and be assured we observe the same value as above.
  movptr(tmpReg, Address(boxReg, 0));
  lock();
  cmpxchgptr(tmpReg, Address(objReg, oopDesc::mark_offset_in_bytes())); // Uses RAX which is box
  // Intention fall-thru into DONE_LABEL

  // DONE_LABEL is a hot target - we'd really like to place it at the
  // start of cache line by padding with NOPs.
  // See the AMD and Intel software optimization manuals for the
  // most efficient "long" NOP encodings.
  // Unfortunately none of our alignment mechanisms suffice.
  bind (CheckSucc);
#else // _LP64
  // It's inflated
  xorptr(boxReg, boxReg);
  orptr(boxReg, Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(recursions)));
  jccb  (Assembler::notZero, DONE_LABEL);
  movptr(boxReg, Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(cxq)));
  orptr(boxReg, Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(EntryList)));
  jccb  (Assembler::notZero, CheckSucc);
  // Without cast to int32_t this style of movptr will destroy r10 which is typically obj.
  movptr(Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)), (int32_t)NULL_WORD);
  jmpb  (DONE_LABEL);

  // Try to avoid passing control into the slow_path ...
  Label LSuccess, LGoSlowPath ;
  bind  (CheckSucc);

  // The following optional optimization can be elided if necessary
  // Effectively: if (succ == null) goto slow path
  // The code reduces the window for a race, however,
  // and thus benefits performance.
  cmpptr(Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(succ)), (int32_t)NULL_WORD);
  jccb  (Assembler::zero, LGoSlowPath);

  xorptr(boxReg, boxReg);
  // Without cast to int32_t this style of movptr will destroy r10 which is typically obj.
  movptr(Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)), (int32_t)NULL_WORD);

  // Memory barrier/fence
  // Dekker pivot point -- fulcrum : ST Owner; MEMBAR; LD Succ
  // Instead of MFENCE we use a dummy locked add of 0 to the top-of-stack.
  // This is faster on Nehalem and AMD Shanghai/Barcelona.
  // See https://blogs.oracle.com/dave/entry/instruction_selection_for_volatile_fences
  // We might also restructure (ST Owner=0;barrier;LD _Succ) to
  // (mov box,0; xchgq box, &m->Owner; LD _succ) .
  lock(); addl(Address(rsp, 0), 0);

  cmpptr(Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(succ)), (int32_t)NULL_WORD);
  jccb  (Assembler::notZero, LSuccess);

  // Rare inopportune interleaving - race.
  // The successor vanished in the small window above.
  // The lock is contended -- (cxq|EntryList) != null -- and there's no apparent successor.
  // We need to ensure progress and succession.
  // Try to reacquire the lock.
  // If that fails then the new owner is responsible for succession and this
  // thread needs to take no further action and can exit via the fast path (success).
  // If the re-acquire succeeds then pass control into the slow path.
  // As implemented, this latter mode is horrible because we generated more
  // coherence traffic on the lock *and* artifically extended the critical section
  // length while by virtue of passing control into the slow path.

  // box is really RAX -- the following CMPXCHG depends on that binding
  // cmpxchg R,[M] is equivalent to rax = CAS(M,rax,R)
  lock();
  cmpxchgptr(r15_thread, Address(tmpReg, OM_OFFSET_NO_MONITOR_VALUE_TAG(owner)));
  // There's no successor so we tried to regrab the lock.
  // If that didn't work, then another thread grabbed the
  // lock so we're done (and exit was a success).
  jccb  (Assembler::notEqual, LSuccess);
  // Intentional fall-through into slow path

  bind  (LGoSlowPath);
  orl   (boxReg, 1);                      // set ICC.ZF=0 to indicate failure
  jmpb  (DONE_LABEL);

  bind  (LSuccess);
  testl (boxReg, 0);                      // set ICC.ZF=1 to indicate success
  jmpb  (DONE_LABEL);

  bind  (Stacked);
  movptr(tmpReg, Address (boxReg, 0));      // re-fetch
  lock();
  cmpxchgptr(tmpReg, Address(objReg, oopDesc::mark_offset_in_bytes())); // Uses RAX which is box

#endif
  bind(DONE_LABEL);
}

//-------------------------------------------------------------------------------------------
// Generic instructions support for use in .ad files C2 code generation

void C2_MacroAssembler::vabsnegd(int opcode, XMMRegister dst, XMMRegister src, Register scr) {
  if (dst != src) {
    movdqu(dst, src);
  }
  if (opcode == Op_AbsVD) {
    andpd(dst, ExternalAddress(StubRoutines::x86::vector_double_sign_mask()), scr);
  } else {
    assert((opcode == Op_NegVD),"opcode should be Op_NegD");
    xorpd(dst, ExternalAddress(StubRoutines::x86::vector_double_sign_flip()), scr);
  }
}

void C2_MacroAssembler::vabsnegd(int opcode, XMMRegister dst, XMMRegister src, int vector_len, Register scr) {
  if (opcode == Op_AbsVD) {
    vandpd(dst, src, ExternalAddress(StubRoutines::x86::vector_double_sign_mask()), vector_len, scr);
  } else {
    assert((opcode == Op_NegVD),"opcode should be Op_NegD");
    vxorpd(dst, src, ExternalAddress(StubRoutines::x86::vector_double_sign_flip()), vector_len, scr);
  }
}

void C2_MacroAssembler::vabsnegf(int opcode, XMMRegister dst, XMMRegister src, Register scr) {
  if (dst != src) {
    movdqu(dst, src);
  }
  if (opcode == Op_AbsVF) {
    andps(dst, ExternalAddress(StubRoutines::x86::vector_float_sign_mask()), scr);
  } else {
    assert((opcode == Op_NegVF),"opcode should be Op_NegF");
    xorps(dst, ExternalAddress(StubRoutines::x86::vector_float_sign_flip()), scr);
  }
}

void C2_MacroAssembler::vabsnegf(int opcode, XMMRegister dst, XMMRegister src, int vector_len, Register scr) {
  if (opcode == Op_AbsVF) {
    vandps(dst, src, ExternalAddress(StubRoutines::x86::vector_float_sign_mask()), vector_len, scr);
  } else {
    assert((opcode == Op_NegVF),"opcode should be Op_NegF");
    vxorps(dst, src, ExternalAddress(StubRoutines::x86::vector_float_sign_flip()), vector_len, scr);
  }
}

void C2_MacroAssembler::vextendbw(bool sign, XMMRegister dst, XMMRegister src) {
  if (sign) {
    pmovsxbw(dst, src);
  } else {
    pmovzxbw(dst, src);
  }
}

void C2_MacroAssembler::vextendbw(bool sign, XMMRegister dst, XMMRegister src, int vector_len) {
  if (sign) {
    vpmovsxbw(dst, src, vector_len);
  } else {
    vpmovzxbw(dst, src, vector_len);
  }
}

void C2_MacroAssembler::vshiftd(int opcode, XMMRegister dst, XMMRegister src) {
  if (opcode == Op_RShiftVI) {
    psrad(dst, src);
  } else if (opcode == Op_LShiftVI) {
    pslld(dst, src);
  } else {
    assert((opcode == Op_URShiftVI),"opcode should be Op_URShiftVI");
    psrld(dst, src);
  }
}

void C2_MacroAssembler::vshiftd(int opcode, XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  if (opcode == Op_RShiftVI) {
    vpsrad(dst, nds, src, vector_len);
  } else if (opcode == Op_LShiftVI) {
    vpslld(dst, nds, src, vector_len);
  } else {
    assert((opcode == Op_URShiftVI),"opcode should be Op_URShiftVI");
    vpsrld(dst, nds, src, vector_len);
  }
}

void C2_MacroAssembler::vshiftw(int opcode, XMMRegister dst, XMMRegister src) {
  if ((opcode == Op_RShiftVS) || (opcode == Op_RShiftVB)) {
    psraw(dst, src);
  } else if ((opcode == Op_LShiftVS) || (opcode == Op_LShiftVB)) {
    psllw(dst, src);
  } else {
    assert(((opcode == Op_URShiftVS) || (opcode == Op_URShiftVB)),"opcode should be one of Op_URShiftVS or Op_URShiftVB");
    psrlw(dst, src);
  }
}

void C2_MacroAssembler::vshiftw(int opcode, XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  if ((opcode == Op_RShiftVS) || (opcode == Op_RShiftVB)) {
    vpsraw(dst, nds, src, vector_len);
  } else if ((opcode == Op_LShiftVS) || (opcode == Op_LShiftVB)) {
    vpsllw(dst, nds, src, vector_len);
  } else {
    assert(((opcode == Op_URShiftVS) || (opcode == Op_URShiftVB)),"opcode should be one of Op_URShiftVS or Op_URShiftVB");
    vpsrlw(dst, nds, src, vector_len);
  }
}

void C2_MacroAssembler::vshiftq(int opcode, XMMRegister dst, XMMRegister src) {
  if (opcode == Op_RShiftVL) {
    psrlq(dst, src);  // using srl to implement sra on pre-avs512 systems
  } else if (opcode == Op_LShiftVL) {
    psllq(dst, src);
  } else {
    assert((opcode == Op_URShiftVL),"opcode should be Op_URShiftVL");
    psrlq(dst, src);
  }
}

void C2_MacroAssembler::vshiftq(int opcode, XMMRegister dst, XMMRegister nds, XMMRegister src, int vector_len) {
  if (opcode == Op_RShiftVL) {
    evpsraq(dst, nds, src, vector_len);
  } else if (opcode == Op_LShiftVL) {
    vpsllq(dst, nds, src, vector_len);
  } else {
    assert((opcode == Op_URShiftVL),"opcode should be Op_URShiftVL");
    vpsrlq(dst, nds, src, vector_len);
  }
}

// Reductions for vectors of ints, longs, floats, and doubles.

void C2_MacroAssembler::reduce_operation_128(int opcode, XMMRegister dst, XMMRegister src) {
  int vector_len = Assembler::AVX_128bit;

  switch (opcode) {
    case Op_AndReductionV:  pand(dst, src); break;
    case Op_OrReductionV:   por (dst, src); break;
    case Op_XorReductionV:  pxor(dst, src); break;

    case Op_AddReductionVF: addss(dst, src); break;
    case Op_AddReductionVD: addsd(dst, src); break;
    case Op_AddReductionVI: paddd(dst, src); break;
    case Op_AddReductionVL: paddq(dst, src); break;

    case Op_MulReductionVF: mulss(dst, src); break;
    case Op_MulReductionVD: mulsd(dst, src); break;
    case Op_MulReductionVI: pmulld(dst, src); break;
    case Op_MulReductionVL: vpmullq(dst, dst, src, vector_len); break;

    default: assert(false, "wrong opcode");
  }
}

void C2_MacroAssembler::reduce_operation_256(int opcode, XMMRegister dst,  XMMRegister src1, XMMRegister src2) {
  int vector_len = Assembler::AVX_256bit;

  switch (opcode) {
    case Op_AndReductionV:  vpand(dst, src1, src2, vector_len); break;
    case Op_OrReductionV:   vpor (dst, src1, src2, vector_len); break;
    case Op_XorReductionV:  vpxor(dst, src1, src2, vector_len); break;

    case Op_AddReductionVI: vpaddd(dst, src1, src2, vector_len); break;
    case Op_AddReductionVL: vpaddq(dst, src1, src2, vector_len); break;

    case Op_MulReductionVI: vpmulld(dst, src1, src2, vector_len); break;
    case Op_MulReductionVL: vpmullq(dst, src1, src2, vector_len); break;

    default: assert(false, "wrong opcode");
  }
}

void C2_MacroAssembler::reduce_fp(int opcode, int vlen,
                                  XMMRegister dst, XMMRegister src,
                                  XMMRegister vtmp1, XMMRegister vtmp2) {
  switch (opcode) {
    case Op_AddReductionVF:
    case Op_MulReductionVF:
      reduceF(opcode, vlen, dst, src, vtmp1, vtmp2);
      break;

    case Op_AddReductionVD:
    case Op_MulReductionVD:
      reduceD(opcode, vlen, dst, src, vtmp1, vtmp2);
      break;

    default: assert(false, "wrong opcode");
  }
}

void C2_MacroAssembler::reduceI(int opcode, int vlen,
                                Register dst, Register src1, XMMRegister src2,
                                XMMRegister vtmp1, XMMRegister vtmp2) {
  switch (vlen) {
    case  2: reduce2I (opcode, dst, src1, src2, vtmp1, vtmp2); break;
    case  4: reduce4I (opcode, dst, src1, src2, vtmp1, vtmp2); break;
    case  8: reduce8I (opcode, dst, src1, src2, vtmp1, vtmp2); break;
    case 16: reduce16I(opcode, dst, src1, src2, vtmp1, vtmp2); break;

    default: assert(false, "wrong vector length");
  }
}

#ifdef _LP64
void C2_MacroAssembler::reduceL(int opcode, int vlen,
                                Register dst, Register src1, XMMRegister src2,
                                XMMRegister vtmp1, XMMRegister vtmp2) {
  switch (vlen) {
    case 2: reduce2L(opcode, dst, src1, src2, vtmp1, vtmp2); break;
    case 4: reduce4L(opcode, dst, src1, src2, vtmp1, vtmp2); break;
    case 8: reduce8L(opcode, dst, src1, src2, vtmp1, vtmp2); break;

    default: assert(false, "wrong vector length");
  }
}
#endif // _LP64

void C2_MacroAssembler::reduceF(int opcode, int vlen, XMMRegister dst, XMMRegister src, XMMRegister vtmp1, XMMRegister vtmp2) {
  switch (vlen) {
    case 2:
      assert(vtmp2 == xnoreg, "");
      reduce2F(opcode, dst, src, vtmp1);
      break;
    case 4:
      assert(vtmp2 == xnoreg, "");
      reduce4F(opcode, dst, src, vtmp1);
      break;
    case 8:
      reduce8F(opcode, dst, src, vtmp1, vtmp2);
      break;
    case 16:
      reduce16F(opcode, dst, src, vtmp1, vtmp2);
      break;
    default: assert(false, "wrong vector length");
  }
}

void C2_MacroAssembler::reduceD(int opcode, int vlen, XMMRegister dst, XMMRegister src, XMMRegister vtmp1, XMMRegister vtmp2) {
  switch (vlen) {
    case 2:
      assert(vtmp2 == xnoreg, "");
      reduce2D(opcode, dst, src, vtmp1);
      break;
    case 4:
      reduce4D(opcode, dst, src, vtmp1, vtmp2);
      break;
    case 8:
      reduce8D(opcode, dst, src, vtmp1, vtmp2);
      break;
    default: assert(false, "wrong vector length");
  }
}

void C2_MacroAssembler::reduce2I(int opcode, Register dst, Register src1, XMMRegister src2, XMMRegister vtmp1, XMMRegister vtmp2) {
  if (opcode == Op_AddReductionVI) {
    if (vtmp1 != src2) {
      movdqu(vtmp1, src2);
    }
    phaddd(vtmp1, vtmp1);
  } else {
    pshufd(vtmp1, src2, 0x1);
    reduce_operation_128(opcode, vtmp1, src2);
  }
  movdl(vtmp2, src1);
  reduce_operation_128(opcode, vtmp1, vtmp2);
  movdl(dst, vtmp1);
}

void C2_MacroAssembler::reduce4I(int opcode, Register dst, Register src1, XMMRegister src2, XMMRegister vtmp1, XMMRegister vtmp2) {
  if (opcode == Op_AddReductionVI) {
    if (vtmp1 != src2) {
      movdqu(vtmp1, src2);
    }
    phaddd(vtmp1, src2);
    reduce2I(opcode, dst, src1, vtmp1, vtmp1, vtmp2);
  } else {
    pshufd(vtmp2, src2, 0xE);
    reduce_operation_128(opcode, vtmp2, src2);
    reduce2I(opcode, dst, src1, vtmp2, vtmp1, vtmp2);
  }
}

void C2_MacroAssembler::reduce8I(int opcode, Register dst, Register src1, XMMRegister src2, XMMRegister vtmp1, XMMRegister vtmp2) {
  if (opcode == Op_AddReductionVI) {
    vphaddd(vtmp1, src2, src2, Assembler::AVX_256bit);
    vextracti128_high(vtmp2, vtmp1);
    vpaddd(vtmp1, vtmp1, vtmp2, Assembler::AVX_128bit);
    reduce2I(opcode, dst, src1, vtmp1, vtmp1, vtmp2);
  } else {
    vextracti128_high(vtmp1, src2);
    reduce_operation_128(opcode, vtmp1, src2);
    reduce4I(opcode, dst, src1, vtmp1, vtmp1, vtmp2);
  }
}

void C2_MacroAssembler::reduce16I(int opcode, Register dst, Register src1, XMMRegister src2, XMMRegister vtmp1, XMMRegister vtmp2) {
  vextracti64x4_high(vtmp2, src2);
  reduce_operation_256(opcode, vtmp2, vtmp2, src2);
  reduce8I(opcode, dst, src1, vtmp2, vtmp1, vtmp2);
}

#ifdef _LP64
void C2_MacroAssembler::reduce2L(int opcode, Register dst, Register src1, XMMRegister src2, XMMRegister vtmp1, XMMRegister vtmp2) {
  pshufd(vtmp2, src2, 0xE);
  reduce_operation_128(opcode, vtmp2, src2);
  movdq(vtmp1, src1);
  reduce_operation_128(opcode, vtmp1, vtmp2);
  movdq(dst, vtmp1);
}

void C2_MacroAssembler::reduce4L(int opcode, Register dst, Register src1, XMMRegister src2, XMMRegister vtmp1, XMMRegister vtmp2) {
  vextracti128_high(vtmp1, src2);
  reduce_operation_128(opcode, vtmp1, src2);
  reduce2L(opcode, dst, src1, vtmp1, vtmp1, vtmp2);
}

void C2_MacroAssembler::reduce8L(int opcode, Register dst, Register src1, XMMRegister src2, XMMRegister vtmp1, XMMRegister vtmp2) {
  vextracti64x4_high(vtmp2, src2);
  reduce_operation_256(opcode, vtmp2, vtmp2, src2);
  reduce4L(opcode, dst, src1, vtmp2, vtmp1, vtmp2);
}
#endif // _LP64

void C2_MacroAssembler::reduce2F(int opcode, XMMRegister dst, XMMRegister src, XMMRegister vtmp) {
  reduce_operation_128(opcode, dst, src);
  pshufd(vtmp, src, 0x1);
  reduce_operation_128(opcode, dst, vtmp);
}

void C2_MacroAssembler::reduce4F(int opcode, XMMRegister dst, XMMRegister src, XMMRegister vtmp) {
  reduce2F(opcode, dst, src, vtmp);
  pshufd(vtmp, src, 0x2);
  reduce_operation_128(opcode, dst, vtmp);
  pshufd(vtmp, src, 0x3);
  reduce_operation_128(opcode, dst, vtmp);
}

void C2_MacroAssembler::reduce8F(int opcode, XMMRegister dst, XMMRegister src, XMMRegister vtmp1, XMMRegister vtmp2) {
  reduce4F(opcode, dst, src, vtmp2);
  vextractf128_high(vtmp2, src);
  reduce4F(opcode, dst, vtmp2, vtmp1);
}

void C2_MacroAssembler::reduce16F(int opcode, XMMRegister dst, XMMRegister src, XMMRegister vtmp1, XMMRegister vtmp2) {
  reduce8F(opcode, dst, src, vtmp1, vtmp2);
  vextracti64x4_high(vtmp1, src);
  reduce8F(opcode, dst, vtmp1, vtmp1, vtmp2);
}

void C2_MacroAssembler::reduce2D(int opcode, XMMRegister dst, XMMRegister src, XMMRegister vtmp) {
  reduce_operation_128(opcode, dst, src);
  pshufd(vtmp, src, 0xE);
  reduce_operation_128(opcode, dst, vtmp);
}

void C2_MacroAssembler::reduce4D(int opcode, XMMRegister dst, XMMRegister src, XMMRegister vtmp1, XMMRegister vtmp2) {
  reduce2D(opcode, dst, src, vtmp2);
  vextractf128_high(vtmp2, src);
  reduce2D(opcode, dst, vtmp2, vtmp1);
}

void C2_MacroAssembler::reduce8D(int opcode, XMMRegister dst, XMMRegister src, XMMRegister vtmp1, XMMRegister vtmp2) {
  reduce4D(opcode, dst, src, vtmp1, vtmp2);
  vextracti64x4_high(vtmp1, src);
  reduce4D(opcode, dst, vtmp1, vtmp1, vtmp2);
}

//-------------------------------------------------------------------------------------------

// IndexOf for constant substrings with size >= 8 chars
// which don't need to be loaded through stack.
void C2_MacroAssembler::string_indexofC8(Register str1, Register str2,
                                         Register cnt1, Register cnt2,
                                         int int_cnt2,  Register result,
                                         XMMRegister vec, Register tmp,
                                         int ae) {
  ShortBranchVerifier sbv(this);
  assert(UseSSE42Intrinsics, "SSE4.2 intrinsics are required");
  assert(ae != StrIntrinsicNode::LU, "Invalid encoding");

  // This method uses the pcmpestri instruction with bound registers
  //   inputs:
  //     xmm - substring
  //     rax - substring length (elements count)
  //     mem - scanned string
  //     rdx - string length (elements count)
  //     0xd - mode: 1100 (substring search) + 01 (unsigned shorts)
  //     0xc - mode: 1100 (substring search) + 00 (unsigned bytes)
  //   outputs:
  //     rcx - matched index in string
  assert(cnt1 == rdx && cnt2 == rax && tmp == rcx, "pcmpestri");
  int mode   = (ae == StrIntrinsicNode::LL) ? 0x0c : 0x0d; // bytes or shorts
  int stride = (ae == StrIntrinsicNode::LL) ? 16 : 8; //UU, UL -> 8
  Address::ScaleFactor scale1 = (ae == StrIntrinsicNode::LL) ? Address::times_1 : Address::times_2;
  Address::ScaleFactor scale2 = (ae == StrIntrinsicNode::UL) ? Address::times_1 : scale1;

  Label RELOAD_SUBSTR, SCAN_TO_SUBSTR, SCAN_SUBSTR,
        RET_FOUND, RET_NOT_FOUND, EXIT, FOUND_SUBSTR,
        MATCH_SUBSTR_HEAD, RELOAD_STR, FOUND_CANDIDATE;

  // Note, inline_string_indexOf() generates checks:
  // if (substr.count > string.count) return -1;
  // if (substr.count == 0) return 0;
  assert(int_cnt2 >= stride, "this code is used only for cnt2 >= 8 chars");

  // Load substring.
  if (ae == StrIntrinsicNode::UL) {
    pmovzxbw(vec, Address(str2, 0));
  } else {
    movdqu(vec, Address(str2, 0));
  }
  movl(cnt2, int_cnt2);
  movptr(result, str1); // string addr

  if (int_cnt2 > stride) {
    jmpb(SCAN_TO_SUBSTR);

    // Reload substr for rescan, this code
    // is executed only for large substrings (> 8 chars)
    bind(RELOAD_SUBSTR);
    if (ae == StrIntrinsicNode::UL) {
      pmovzxbw(vec, Address(str2, 0));
    } else {
      movdqu(vec, Address(str2, 0));
    }
    negptr(cnt2); // Jumped here with negative cnt2, convert to positive

    bind(RELOAD_STR);
    // We came here after the beginning of the substring was
    // matched but the rest of it was not so we need to search
    // again. Start from the next element after the previous match.

    // cnt2 is number of substring reminding elements and
    // cnt1 is number of string reminding elements when cmp failed.
    // Restored cnt1 = cnt1 - cnt2 + int_cnt2
    subl(cnt1, cnt2);
    addl(cnt1, int_cnt2);
    movl(cnt2, int_cnt2); // Now restore cnt2

    decrementl(cnt1);     // Shift to next element
    cmpl(cnt1, cnt2);
    jcc(Assembler::negative, RET_NOT_FOUND);  // Left less then substring

    addptr(result, (1<<scale1));

  } // (int_cnt2 > 8)

  // Scan string for start of substr in 16-byte vectors
  bind(SCAN_TO_SUBSTR);
  pcmpestri(vec, Address(result, 0), mode);
  jccb(Assembler::below, FOUND_CANDIDATE);   // CF == 1
  subl(cnt1, stride);
  jccb(Assembler::lessEqual, RET_NOT_FOUND); // Scanned full string
  cmpl(cnt1, cnt2);
  jccb(Assembler::negative, RET_NOT_FOUND);  // Left less then substring
  addptr(result, 16);
  jmpb(SCAN_TO_SUBSTR);

  // Found a potential substr
  bind(FOUND_CANDIDATE);
  // Matched whole vector if first element matched (tmp(rcx) == 0).
  if (int_cnt2 == stride) {
    jccb(Assembler::overflow, RET_FOUND);    // OF == 1
  } else { // int_cnt2 > 8
    jccb(Assembler::overflow, FOUND_SUBSTR);
  }
  // After pcmpestri tmp(rcx) contains matched element index
  // Compute start addr of substr
  lea(result, Address(result, tmp, scale1));

  // Make sure string is still long enough
  subl(cnt1, tmp);
  cmpl(cnt1, cnt2);
  if (int_cnt2 == stride) {
    jccb(Assembler::greaterEqual, SCAN_TO_SUBSTR);
  } else { // int_cnt2 > 8
    jccb(Assembler::greaterEqual, MATCH_SUBSTR_HEAD);
  }
  // Left less then substring.

  bind(RET_NOT_FOUND);
  movl(result, -1);
  jmp(EXIT);

  if (int_cnt2 > stride) {
    // This code is optimized for the case when whole substring
    // is matched if its head is matched.
    bind(MATCH_SUBSTR_HEAD);
    pcmpestri(vec, Address(result, 0), mode);
    // Reload only string if does not match
    jcc(Assembler::noOverflow, RELOAD_STR); // OF == 0

    Label CONT_SCAN_SUBSTR;
    // Compare the rest of substring (> 8 chars).
    bind(FOUND_SUBSTR);
    // First 8 chars are already matched.
    negptr(cnt2);
    addptr(cnt2, stride);

    bind(SCAN_SUBSTR);
    subl(cnt1, stride);
    cmpl(cnt2, -stride); // Do not read beyond substring
    jccb(Assembler::lessEqual, CONT_SCAN_SUBSTR);
    // Back-up strings to avoid reading beyond substring:
    // cnt1 = cnt1 - cnt2 + 8
    addl(cnt1, cnt2); // cnt2 is negative
    addl(cnt1, stride);
    movl(cnt2, stride); negptr(cnt2);
    bind(CONT_SCAN_SUBSTR);
    if (int_cnt2 < (int)G) {
      int tail_off1 = int_cnt2<<scale1;
      int tail_off2 = int_cnt2<<scale2;
      if (ae == StrIntrinsicNode::UL) {
        pmovzxbw(vec, Address(str2, cnt2, scale2, tail_off2));
      } else {
        movdqu(vec, Address(str2, cnt2, scale2, tail_off2));
      }
      pcmpestri(vec, Address(result, cnt2, scale1, tail_off1), mode);
    } else {
      // calculate index in register to avoid integer overflow (int_cnt2*2)
      movl(tmp, int_cnt2);
      addptr(tmp, cnt2);
      if (ae == StrIntrinsicNode::UL) {
        pmovzxbw(vec, Address(str2, tmp, scale2, 0));
      } else {
        movdqu(vec, Address(str2, tmp, scale2, 0));
      }
      pcmpestri(vec, Address(result, tmp, scale1, 0), mode);
    }
    // Need to reload strings pointers if not matched whole vector
    jcc(Assembler::noOverflow, RELOAD_SUBSTR); // OF == 0
    addptr(cnt2, stride);
    jcc(Assembler::negative, SCAN_SUBSTR);
    // Fall through if found full substring

  } // (int_cnt2 > 8)

  bind(RET_FOUND);
  // Found result if we matched full small substring.
  // Compute substr offset
  subptr(result, str1);
  if (ae == StrIntrinsicNode::UU || ae == StrIntrinsicNode::UL) {
    shrl(result, 1); // index
  }
  bind(EXIT);

} // string_indexofC8

// Small strings are loaded through stack if they cross page boundary.
void C2_MacroAssembler::string_indexof(Register str1, Register str2,
                                       Register cnt1, Register cnt2,
                                       int int_cnt2,  Register result,
                                       XMMRegister vec, Register tmp,
                                       int ae) {
  ShortBranchVerifier sbv(this);
  assert(UseSSE42Intrinsics, "SSE4.2 intrinsics are required");
  assert(ae != StrIntrinsicNode::LU, "Invalid encoding");

  //
  // int_cnt2 is length of small (< 8 chars) constant substring
  // or (-1) for non constant substring in which case its length
  // is in cnt2 register.
  //
  // Note, inline_string_indexOf() generates checks:
  // if (substr.count > string.count) return -1;
  // if (substr.count == 0) return 0;
  //
  int stride = (ae == StrIntrinsicNode::LL) ? 16 : 8; //UU, UL -> 8
  assert(int_cnt2 == -1 || (0 < int_cnt2 && int_cnt2 < stride), "should be != 0");
  // This method uses the pcmpestri instruction with bound registers
  //   inputs:
  //     xmm - substring
  //     rax - substring length (elements count)
  //     mem - scanned string
  //     rdx - string length (elements count)
  //     0xd - mode: 1100 (substring search) + 01 (unsigned shorts)
  //     0xc - mode: 1100 (substring search) + 00 (unsigned bytes)
  //   outputs:
  //     rcx - matched index in string
  assert(cnt1 == rdx && cnt2 == rax && tmp == rcx, "pcmpestri");
  int mode = (ae == StrIntrinsicNode::LL) ? 0x0c : 0x0d; // bytes or shorts
  Address::ScaleFactor scale1 = (ae == StrIntrinsicNode::LL) ? Address::times_1 : Address::times_2;
  Address::ScaleFactor scale2 = (ae == StrIntrinsicNode::UL) ? Address::times_1 : scale1;

  Label RELOAD_SUBSTR, SCAN_TO_SUBSTR, SCAN_SUBSTR, ADJUST_STR,
        RET_FOUND, RET_NOT_FOUND, CLEANUP, FOUND_SUBSTR,
        FOUND_CANDIDATE;

  { //========================================================
    // We don't know where these strings are located
    // and we can't read beyond them. Load them through stack.
    Label BIG_STRINGS, CHECK_STR, COPY_SUBSTR, COPY_STR;

    movptr(tmp, rsp); // save old SP

    if (int_cnt2 > 0) {     // small (< 8 chars) constant substring
      if (int_cnt2 == (1>>scale2)) { // One byte
        assert((ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UL), "Only possible for latin1 encoding");
        load_unsigned_byte(result, Address(str2, 0));
        movdl(vec, result); // move 32 bits
      } else if (ae == StrIntrinsicNode::LL && int_cnt2 == 3) {  // Three bytes
        // Not enough header space in 32-bit VM: 12+3 = 15.
        movl(result, Address(str2, -1));
        shrl(result, 8);
        movdl(vec, result); // move 32 bits
      } else if (ae != StrIntrinsicNode::UL && int_cnt2 == (2>>scale2)) {  // One char
        load_unsigned_short(result, Address(str2, 0));
        movdl(vec, result); // move 32 bits
      } else if (ae != StrIntrinsicNode::UL && int_cnt2 == (4>>scale2)) { // Two chars
        movdl(vec, Address(str2, 0)); // move 32 bits
      } else if (ae != StrIntrinsicNode::UL && int_cnt2 == (8>>scale2)) { // Four chars
        movq(vec, Address(str2, 0));  // move 64 bits
      } else { // cnt2 = { 3, 5, 6, 7 } || (ae == StrIntrinsicNode::UL && cnt2 ={2, ..., 7})
        // Array header size is 12 bytes in 32-bit VM
        // + 6 bytes for 3 chars == 18 bytes,
        // enough space to load vec and shift.
        assert(HeapWordSize*TypeArrayKlass::header_size() >= 12,"sanity");
        if (ae == StrIntrinsicNode::UL) {
          int tail_off = int_cnt2-8;
          pmovzxbw(vec, Address(str2, tail_off));
          psrldq(vec, -2*tail_off);
        }
        else {
          int tail_off = int_cnt2*(1<<scale2);
          movdqu(vec, Address(str2, tail_off-16));
          psrldq(vec, 16-tail_off);
        }
      }
    } else { // not constant substring
      cmpl(cnt2, stride);
      jccb(Assembler::aboveEqual, BIG_STRINGS); // Both strings are big enough

      // We can read beyond string if srt+16 does not cross page boundary
      // since heaps are aligned and mapped by pages.
      assert(os::vm_page_size() < (int)G, "default page should be small");
      movl(result, str2); // We need only low 32 bits
      andl(result, (os::vm_page_size()-1));
      cmpl(result, (os::vm_page_size()-16));
      jccb(Assembler::belowEqual, CHECK_STR);

      // Move small strings to stack to allow load 16 bytes into vec.
      subptr(rsp, 16);
      int stk_offset = wordSize-(1<<scale2);
      push(cnt2);

      bind(COPY_SUBSTR);
      if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UL) {
        load_unsigned_byte(result, Address(str2, cnt2, scale2, -1));
        movb(Address(rsp, cnt2, scale2, stk_offset), result);
      } else if (ae == StrIntrinsicNode::UU) {
        load_unsigned_short(result, Address(str2, cnt2, scale2, -2));
        movw(Address(rsp, cnt2, scale2, stk_offset), result);
      }
      decrement(cnt2);
      jccb(Assembler::notZero, COPY_SUBSTR);

      pop(cnt2);
      movptr(str2, rsp);  // New substring address
    } // non constant

    bind(CHECK_STR);
    cmpl(cnt1, stride);
    jccb(Assembler::aboveEqual, BIG_STRINGS);

    // Check cross page boundary.
    movl(result, str1); // We need only low 32 bits
    andl(result, (os::vm_page_size()-1));
    cmpl(result, (os::vm_page_size()-16));
    jccb(Assembler::belowEqual, BIG_STRINGS);

    subptr(rsp, 16);
    int stk_offset = -(1<<scale1);
    if (int_cnt2 < 0) { // not constant
      push(cnt2);
      stk_offset += wordSize;
    }
    movl(cnt2, cnt1);

    bind(COPY_STR);
    if (ae == StrIntrinsicNode::LL) {
      load_unsigned_byte(result, Address(str1, cnt2, scale1, -1));
      movb(Address(rsp, cnt2, scale1, stk_offset), result);
    } else {
      load_unsigned_short(result, Address(str1, cnt2, scale1, -2));
      movw(Address(rsp, cnt2, scale1, stk_offset), result);
    }
    decrement(cnt2);
    jccb(Assembler::notZero, COPY_STR);

    if (int_cnt2 < 0) { // not constant
      pop(cnt2);
    }
    movptr(str1, rsp);  // New string address

    bind(BIG_STRINGS);
    // Load substring.
    if (int_cnt2 < 0) { // -1
      if (ae == StrIntrinsicNode::UL) {
        pmovzxbw(vec, Address(str2, 0));
      } else {
        movdqu(vec, Address(str2, 0));
      }
      push(cnt2);       // substr count
      push(str2);       // substr addr
      push(str1);       // string addr
    } else {
      // Small (< 8 chars) constant substrings are loaded already.
      movl(cnt2, int_cnt2);
    }
    push(tmp);  // original SP

  } // Finished loading

  //========================================================
  // Start search
  //

  movptr(result, str1); // string addr

  if (int_cnt2  < 0) {  // Only for non constant substring
    jmpb(SCAN_TO_SUBSTR);

    // SP saved at sp+0
    // String saved at sp+1*wordSize
    // Substr saved at sp+2*wordSize
    // Substr count saved at sp+3*wordSize

    // Reload substr for rescan, this code
    // is executed only for large substrings (> 8 chars)
    bind(RELOAD_SUBSTR);
    movptr(str2, Address(rsp, 2*wordSize));
    movl(cnt2, Address(rsp, 3*wordSize));
    if (ae == StrIntrinsicNode::UL) {
      pmovzxbw(vec, Address(str2, 0));
    } else {
      movdqu(vec, Address(str2, 0));
    }
    // We came here after the beginning of the substring was
    // matched but the rest of it was not so we need to search
    // again. Start from the next element after the previous match.
    subptr(str1, result); // Restore counter
    if (ae == StrIntrinsicNode::UU || ae == StrIntrinsicNode::UL) {
      shrl(str1, 1);
    }
    addl(cnt1, str1);
    decrementl(cnt1);   // Shift to next element
    cmpl(cnt1, cnt2);
    jcc(Assembler::negative, RET_NOT_FOUND);  // Left less then substring

    addptr(result, (1<<scale1));
  } // non constant

  // Scan string for start of substr in 16-byte vectors
  bind(SCAN_TO_SUBSTR);
  assert(cnt1 == rdx && cnt2 == rax && tmp == rcx, "pcmpestri");
  pcmpestri(vec, Address(result, 0), mode);
  jccb(Assembler::below, FOUND_CANDIDATE);   // CF == 1
  subl(cnt1, stride);
  jccb(Assembler::lessEqual, RET_NOT_FOUND); // Scanned full string
  cmpl(cnt1, cnt2);
  jccb(Assembler::negative, RET_NOT_FOUND);  // Left less then substring
  addptr(result, 16);

  bind(ADJUST_STR);
  cmpl(cnt1, stride); // Do not read beyond string
  jccb(Assembler::greaterEqual, SCAN_TO_SUBSTR);
  // Back-up string to avoid reading beyond string.
  lea(result, Address(result, cnt1, scale1, -16));
  movl(cnt1, stride);
  jmpb(SCAN_TO_SUBSTR);

  // Found a potential substr
  bind(FOUND_CANDIDATE);
  // After pcmpestri tmp(rcx) contains matched element index

  // Make sure string is still long enough
  subl(cnt1, tmp);
  cmpl(cnt1, cnt2);
  jccb(Assembler::greaterEqual, FOUND_SUBSTR);
  // Left less then substring.

  bind(RET_NOT_FOUND);
  movl(result, -1);
  jmp(CLEANUP);

  bind(FOUND_SUBSTR);
  // Compute start addr of substr
  lea(result, Address(result, tmp, scale1));
  if (int_cnt2 > 0) { // Constant substring
    // Repeat search for small substring (< 8 chars)
    // from new point without reloading substring.
    // Have to check that we don't read beyond string.
    cmpl(tmp, stride-int_cnt2);
    jccb(Assembler::greater, ADJUST_STR);
    // Fall through if matched whole substring.
  } else { // non constant
    assert(int_cnt2 == -1, "should be != 0");

    addl(tmp, cnt2);
    // Found result if we matched whole substring.
    cmpl(tmp, stride);
    jcc(Assembler::lessEqual, RET_FOUND);

    // Repeat search for small substring (<= 8 chars)
    // from new point 'str1' without reloading substring.
    cmpl(cnt2, stride);
    // Have to check that we don't read beyond string.
    jccb(Assembler::lessEqual, ADJUST_STR);

    Label CHECK_NEXT, CONT_SCAN_SUBSTR, RET_FOUND_LONG;
    // Compare the rest of substring (> 8 chars).
    movptr(str1, result);

    cmpl(tmp, cnt2);
    // First 8 chars are already matched.
    jccb(Assembler::equal, CHECK_NEXT);

    bind(SCAN_SUBSTR);
    pcmpestri(vec, Address(str1, 0), mode);
    // Need to reload strings pointers if not matched whole vector
    jcc(Assembler::noOverflow, RELOAD_SUBSTR); // OF == 0

    bind(CHECK_NEXT);
    subl(cnt2, stride);
    jccb(Assembler::lessEqual, RET_FOUND_LONG); // Found full substring
    addptr(str1, 16);
    if (ae == StrIntrinsicNode::UL) {
      addptr(str2, 8);
    } else {
      addptr(str2, 16);
    }
    subl(cnt1, stride);
    cmpl(cnt2, stride); // Do not read beyond substring
    jccb(Assembler::greaterEqual, CONT_SCAN_SUBSTR);
    // Back-up strings to avoid reading beyond substring.

    if (ae == StrIntrinsicNode::UL) {
      lea(str2, Address(str2, cnt2, scale2, -8));
      lea(str1, Address(str1, cnt2, scale1, -16));
    } else {
      lea(str2, Address(str2, cnt2, scale2, -16));
      lea(str1, Address(str1, cnt2, scale1, -16));
    }
    subl(cnt1, cnt2);
    movl(cnt2, stride);
    addl(cnt1, stride);
    bind(CONT_SCAN_SUBSTR);
    if (ae == StrIntrinsicNode::UL) {
      pmovzxbw(vec, Address(str2, 0));
    } else {
      movdqu(vec, Address(str2, 0));
    }
    jmp(SCAN_SUBSTR);

    bind(RET_FOUND_LONG);
    movptr(str1, Address(rsp, wordSize));
  } // non constant

  bind(RET_FOUND);
  // Compute substr offset
  subptr(result, str1);
  if (ae == StrIntrinsicNode::UU || ae == StrIntrinsicNode::UL) {
    shrl(result, 1); // index
  }
  bind(CLEANUP);
  pop(rsp); // restore SP

} // string_indexof

void C2_MacroAssembler::string_indexof_char(Register str1, Register cnt1, Register ch, Register result,
                                            XMMRegister vec1, XMMRegister vec2, XMMRegister vec3, Register tmp) {
  ShortBranchVerifier sbv(this);
  assert(UseSSE42Intrinsics, "SSE4.2 intrinsics are required");

  int stride = 8;

  Label FOUND_CHAR, SCAN_TO_CHAR, SCAN_TO_CHAR_LOOP,
        SCAN_TO_8_CHAR, SCAN_TO_8_CHAR_LOOP, SCAN_TO_16_CHAR_LOOP,
        RET_NOT_FOUND, SCAN_TO_8_CHAR_INIT,
        FOUND_SEQ_CHAR, DONE_LABEL;

  movptr(result, str1);
  if (UseAVX >= 2) {
    cmpl(cnt1, stride);
    jcc(Assembler::less, SCAN_TO_CHAR);
    cmpl(cnt1, 2*stride);
    jcc(Assembler::less, SCAN_TO_8_CHAR_INIT);
    movdl(vec1, ch);
    vpbroadcastw(vec1, vec1, Assembler::AVX_256bit);
    vpxor(vec2, vec2);
    movl(tmp, cnt1);
    andl(tmp, 0xFFFFFFF0);  //vector count (in chars)
    andl(cnt1,0x0000000F);  //tail count (in chars)

    bind(SCAN_TO_16_CHAR_LOOP);
    vmovdqu(vec3, Address(result, 0));
    vpcmpeqw(vec3, vec3, vec1, 1);
    vptest(vec2, vec3);
    jcc(Assembler::carryClear, FOUND_CHAR);
    addptr(result, 32);
    subl(tmp, 2*stride);
    jcc(Assembler::notZero, SCAN_TO_16_CHAR_LOOP);
    jmp(SCAN_TO_8_CHAR);
    bind(SCAN_TO_8_CHAR_INIT);
    movdl(vec1, ch);
    pshuflw(vec1, vec1, 0x00);
    pshufd(vec1, vec1, 0);
    pxor(vec2, vec2);
  }
  bind(SCAN_TO_8_CHAR);
  cmpl(cnt1, stride);
  jcc(Assembler::less, SCAN_TO_CHAR);
  if (UseAVX < 2) {
    movdl(vec1, ch);
    pshuflw(vec1, vec1, 0x00);
    pshufd(vec1, vec1, 0);
    pxor(vec2, vec2);
  }
  movl(tmp, cnt1);
  andl(tmp, 0xFFFFFFF8);  //vector count (in chars)
  andl(cnt1,0x00000007);  //tail count (in chars)

  bind(SCAN_TO_8_CHAR_LOOP);
  movdqu(vec3, Address(result, 0));
  pcmpeqw(vec3, vec1);
  ptest(vec2, vec3);
  jcc(Assembler::carryClear, FOUND_CHAR);
  addptr(result, 16);
  subl(tmp, stride);
  jcc(Assembler::notZero, SCAN_TO_8_CHAR_LOOP);
  bind(SCAN_TO_CHAR);
  testl(cnt1, cnt1);
  jcc(Assembler::zero, RET_NOT_FOUND);
  bind(SCAN_TO_CHAR_LOOP);
  load_unsigned_short(tmp, Address(result, 0));
  cmpl(ch, tmp);
  jccb(Assembler::equal, FOUND_SEQ_CHAR);
  addptr(result, 2);
  subl(cnt1, 1);
  jccb(Assembler::zero, RET_NOT_FOUND);
  jmp(SCAN_TO_CHAR_LOOP);

  bind(RET_NOT_FOUND);
  movl(result, -1);
  jmpb(DONE_LABEL);

  bind(FOUND_CHAR);
  if (UseAVX >= 2) {
    vpmovmskb(tmp, vec3);
  } else {
    pmovmskb(tmp, vec3);
  }
  bsfl(ch, tmp);
  addl(result, ch);

  bind(FOUND_SEQ_CHAR);
  subptr(result, str1);
  shrl(result, 1);

  bind(DONE_LABEL);
} // string_indexof_char

// helper function for string_compare
void C2_MacroAssembler::load_next_elements(Register elem1, Register elem2, Register str1, Register str2,
                                           Address::ScaleFactor scale, Address::ScaleFactor scale1,
                                           Address::ScaleFactor scale2, Register index, int ae) {
  if (ae == StrIntrinsicNode::LL) {
    load_unsigned_byte(elem1, Address(str1, index, scale, 0));
    load_unsigned_byte(elem2, Address(str2, index, scale, 0));
  } else if (ae == StrIntrinsicNode::UU) {
    load_unsigned_short(elem1, Address(str1, index, scale, 0));
    load_unsigned_short(elem2, Address(str2, index, scale, 0));
  } else {
    load_unsigned_byte(elem1, Address(str1, index, scale1, 0));
    load_unsigned_short(elem2, Address(str2, index, scale2, 0));
  }
}

// Compare strings, used for char[] and byte[].
void C2_MacroAssembler::string_compare(Register str1, Register str2,
                                       Register cnt1, Register cnt2, Register result,
                                       XMMRegister vec1, int ae) {
  ShortBranchVerifier sbv(this);
  Label LENGTH_DIFF_LABEL, POP_LABEL, DONE_LABEL, WHILE_HEAD_LABEL;
  Label COMPARE_WIDE_VECTORS_LOOP_FAILED;  // used only _LP64 && AVX3
  int stride, stride2, adr_stride, adr_stride1, adr_stride2;
  int stride2x2 = 0x40;
  Address::ScaleFactor scale = Address::no_scale;
  Address::ScaleFactor scale1 = Address::no_scale;
  Address::ScaleFactor scale2 = Address::no_scale;

  if (ae != StrIntrinsicNode::LL) {
    stride2x2 = 0x20;
  }

  if (ae == StrIntrinsicNode::LU || ae == StrIntrinsicNode::UL) {
    shrl(cnt2, 1);
  }
  // Compute the minimum of the string lengths and the
  // difference of the string lengths (stack).
  // Do the conditional move stuff
  movl(result, cnt1);
  subl(cnt1, cnt2);
  push(cnt1);
  cmov32(Assembler::lessEqual, cnt2, result);    // cnt2 = min(cnt1, cnt2)

  // Is the minimum length zero?
  testl(cnt2, cnt2);
  jcc(Assembler::zero, LENGTH_DIFF_LABEL);
  if (ae == StrIntrinsicNode::LL) {
    // Load first bytes
    load_unsigned_byte(result, Address(str1, 0));  // result = str1[0]
    load_unsigned_byte(cnt1, Address(str2, 0));    // cnt1   = str2[0]
  } else if (ae == StrIntrinsicNode::UU) {
    // Load first characters
    load_unsigned_short(result, Address(str1, 0));
    load_unsigned_short(cnt1, Address(str2, 0));
  } else {
    load_unsigned_byte(result, Address(str1, 0));
    load_unsigned_short(cnt1, Address(str2, 0));
  }
  subl(result, cnt1);
  jcc(Assembler::notZero,  POP_LABEL);

  if (ae == StrIntrinsicNode::UU) {
    // Divide length by 2 to get number of chars
    shrl(cnt2, 1);
  }
  cmpl(cnt2, 1);
  jcc(Assembler::equal, LENGTH_DIFF_LABEL);

  // Check if the strings start at the same location and setup scale and stride
  if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
    cmpptr(str1, str2);
    jcc(Assembler::equal, LENGTH_DIFF_LABEL);
    if (ae == StrIntrinsicNode::LL) {
      scale = Address::times_1;
      stride = 16;
    } else {
      scale = Address::times_2;
      stride = 8;
    }
  } else {
    scale1 = Address::times_1;
    scale2 = Address::times_2;
    // scale not used
    stride = 8;
  }

  if (UseAVX >= 2 && UseSSE42Intrinsics) {
    Label COMPARE_WIDE_VECTORS, VECTOR_NOT_EQUAL, COMPARE_WIDE_TAIL, COMPARE_SMALL_STR;
    Label COMPARE_WIDE_VECTORS_LOOP, COMPARE_16_CHARS, COMPARE_INDEX_CHAR;
    Label COMPARE_WIDE_VECTORS_LOOP_AVX2;
    Label COMPARE_TAIL_LONG;
    Label COMPARE_WIDE_VECTORS_LOOP_AVX3;  // used only _LP64 && AVX3

    int pcmpmask = 0x19;
    if (ae == StrIntrinsicNode::LL) {
      pcmpmask &= ~0x01;
    }

    // Setup to compare 16-chars (32-bytes) vectors,
    // start from first character again because it has aligned address.
    if (ae == StrIntrinsicNode::LL) {
      stride2 = 32;
    } else {
      stride2 = 16;
    }
    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      adr_stride = stride << scale;
    } else {
      adr_stride1 = 8;  //stride << scale1;
      adr_stride2 = 16; //stride << scale2;
    }

    assert(result == rax && cnt2 == rdx && cnt1 == rcx, "pcmpestri");
    // rax and rdx are used by pcmpestri as elements counters
    movl(result, cnt2);
    andl(cnt2, ~(stride2-1));   // cnt2 holds the vector count
    jcc(Assembler::zero, COMPARE_TAIL_LONG);

    // fast path : compare first 2 8-char vectors.
    bind(COMPARE_16_CHARS);
    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      movdqu(vec1, Address(str1, 0));
    } else {
      pmovzxbw(vec1, Address(str1, 0));
    }
    pcmpestri(vec1, Address(str2, 0), pcmpmask);
    jccb(Assembler::below, COMPARE_INDEX_CHAR);

    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      movdqu(vec1, Address(str1, adr_stride));
      pcmpestri(vec1, Address(str2, adr_stride), pcmpmask);
    } else {
      pmovzxbw(vec1, Address(str1, adr_stride1));
      pcmpestri(vec1, Address(str2, adr_stride2), pcmpmask);
    }
    jccb(Assembler::aboveEqual, COMPARE_WIDE_VECTORS);
    addl(cnt1, stride);

    // Compare the characters at index in cnt1
    bind(COMPARE_INDEX_CHAR); // cnt1 has the offset of the mismatching character
    load_next_elements(result, cnt2, str1, str2, scale, scale1, scale2, cnt1, ae);
    subl(result, cnt2);
    jmp(POP_LABEL);

    // Setup the registers to start vector comparison loop
    bind(COMPARE_WIDE_VECTORS);
    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      lea(str1, Address(str1, result, scale));
      lea(str2, Address(str2, result, scale));
    } else {
      lea(str1, Address(str1, result, scale1));
      lea(str2, Address(str2, result, scale2));
    }
    subl(result, stride2);
    subl(cnt2, stride2);
    jcc(Assembler::zero, COMPARE_WIDE_TAIL);
    negptr(result);

    //  In a loop, compare 16-chars (32-bytes) at once using (vpxor+vptest)
    bind(COMPARE_WIDE_VECTORS_LOOP);

#ifdef _LP64
    if ((AVX3Threshold == 0) && VM_Version::supports_avx512vlbw()) { // trying 64 bytes fast loop
      cmpl(cnt2, stride2x2);
      jccb(Assembler::below, COMPARE_WIDE_VECTORS_LOOP_AVX2);
      testl(cnt2, stride2x2-1);   // cnt2 holds the vector count
      jccb(Assembler::notZero, COMPARE_WIDE_VECTORS_LOOP_AVX2);   // means we cannot subtract by 0x40

      bind(COMPARE_WIDE_VECTORS_LOOP_AVX3); // the hottest loop
      if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
        evmovdquq(vec1, Address(str1, result, scale), Assembler::AVX_512bit);
        evpcmpeqb(k7, vec1, Address(str2, result, scale), Assembler::AVX_512bit); // k7 == 11..11, if operands equal, otherwise k7 has some 0
      } else {
        vpmovzxbw(vec1, Address(str1, result, scale1), Assembler::AVX_512bit);
        evpcmpeqb(k7, vec1, Address(str2, result, scale2), Assembler::AVX_512bit); // k7 == 11..11, if operands equal, otherwise k7 has some 0
      }
      kortestql(k7, k7);
      jcc(Assembler::aboveEqual, COMPARE_WIDE_VECTORS_LOOP_FAILED);     // miscompare
      addptr(result, stride2x2);  // update since we already compared at this addr
      subl(cnt2, stride2x2);      // and sub the size too
      jccb(Assembler::notZero, COMPARE_WIDE_VECTORS_LOOP_AVX3);

      vpxor(vec1, vec1);
      jmpb(COMPARE_WIDE_TAIL);
    }//if (VM_Version::supports_avx512vlbw())
#endif // _LP64


    bind(COMPARE_WIDE_VECTORS_LOOP_AVX2);
    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      vmovdqu(vec1, Address(str1, result, scale));
      vpxor(vec1, Address(str2, result, scale));
    } else {
      vpmovzxbw(vec1, Address(str1, result, scale1), Assembler::AVX_256bit);
      vpxor(vec1, Address(str2, result, scale2));
    }
    vptest(vec1, vec1);
    jcc(Assembler::notZero, VECTOR_NOT_EQUAL);
    addptr(result, stride2);
    subl(cnt2, stride2);
    jcc(Assembler::notZero, COMPARE_WIDE_VECTORS_LOOP);
    // clean upper bits of YMM registers
    vpxor(vec1, vec1);

    // compare wide vectors tail
    bind(COMPARE_WIDE_TAIL);
    testptr(result, result);
    jcc(Assembler::zero, LENGTH_DIFF_LABEL);

    movl(result, stride2);
    movl(cnt2, result);
    negptr(result);
    jmp(COMPARE_WIDE_VECTORS_LOOP_AVX2);

    // Identifies the mismatching (higher or lower)16-bytes in the 32-byte vectors.
    bind(VECTOR_NOT_EQUAL);
    // clean upper bits of YMM registers
    vpxor(vec1, vec1);
    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      lea(str1, Address(str1, result, scale));
      lea(str2, Address(str2, result, scale));
    } else {
      lea(str1, Address(str1, result, scale1));
      lea(str2, Address(str2, result, scale2));
    }
    jmp(COMPARE_16_CHARS);

    // Compare tail chars, length between 1 to 15 chars
    bind(COMPARE_TAIL_LONG);
    movl(cnt2, result);
    cmpl(cnt2, stride);
    jcc(Assembler::less, COMPARE_SMALL_STR);

    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      movdqu(vec1, Address(str1, 0));
    } else {
      pmovzxbw(vec1, Address(str1, 0));
    }
    pcmpestri(vec1, Address(str2, 0), pcmpmask);
    jcc(Assembler::below, COMPARE_INDEX_CHAR);
    subptr(cnt2, stride);
    jcc(Assembler::zero, LENGTH_DIFF_LABEL);
    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      lea(str1, Address(str1, result, scale));
      lea(str2, Address(str2, result, scale));
    } else {
      lea(str1, Address(str1, result, scale1));
      lea(str2, Address(str2, result, scale2));
    }
    negptr(cnt2);
    jmpb(WHILE_HEAD_LABEL);

    bind(COMPARE_SMALL_STR);
  } else if (UseSSE42Intrinsics) {
    Label COMPARE_WIDE_VECTORS, VECTOR_NOT_EQUAL, COMPARE_TAIL;
    int pcmpmask = 0x19;
    // Setup to compare 8-char (16-byte) vectors,
    // start from first character again because it has aligned address.
    movl(result, cnt2);
    andl(cnt2, ~(stride - 1));   // cnt2 holds the vector count
    if (ae == StrIntrinsicNode::LL) {
      pcmpmask &= ~0x01;
    }
    jcc(Assembler::zero, COMPARE_TAIL);
    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      lea(str1, Address(str1, result, scale));
      lea(str2, Address(str2, result, scale));
    } else {
      lea(str1, Address(str1, result, scale1));
      lea(str2, Address(str2, result, scale2));
    }
    negptr(result);

    // pcmpestri
    //   inputs:
    //     vec1- substring
    //     rax - negative string length (elements count)
    //     mem - scanned string
    //     rdx - string length (elements count)
    //     pcmpmask - cmp mode: 11000 (string compare with negated result)
    //               + 00 (unsigned bytes) or  + 01 (unsigned shorts)
    //   outputs:
    //     rcx - first mismatched element index
    assert(result == rax && cnt2 == rdx && cnt1 == rcx, "pcmpestri");

    bind(COMPARE_WIDE_VECTORS);
    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      movdqu(vec1, Address(str1, result, scale));
      pcmpestri(vec1, Address(str2, result, scale), pcmpmask);
    } else {
      pmovzxbw(vec1, Address(str1, result, scale1));
      pcmpestri(vec1, Address(str2, result, scale2), pcmpmask);
    }
    // After pcmpestri cnt1(rcx) contains mismatched element index

    jccb(Assembler::below, VECTOR_NOT_EQUAL);  // CF==1
    addptr(result, stride);
    subptr(cnt2, stride);
    jccb(Assembler::notZero, COMPARE_WIDE_VECTORS);

    // compare wide vectors tail
    testptr(result, result);
    jcc(Assembler::zero, LENGTH_DIFF_LABEL);

    movl(cnt2, stride);
    movl(result, stride);
    negptr(result);
    if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
      movdqu(vec1, Address(str1, result, scale));
      pcmpestri(vec1, Address(str2, result, scale), pcmpmask);
    } else {
      pmovzxbw(vec1, Address(str1, result, scale1));
      pcmpestri(vec1, Address(str2, result, scale2), pcmpmask);
    }
    jccb(Assembler::aboveEqual, LENGTH_DIFF_LABEL);

    // Mismatched characters in the vectors
    bind(VECTOR_NOT_EQUAL);
    addptr(cnt1, result);
    load_next_elements(result, cnt2, str1, str2, scale, scale1, scale2, cnt1, ae);
    subl(result, cnt2);
    jmpb(POP_LABEL);

    bind(COMPARE_TAIL); // limit is zero
    movl(cnt2, result);
    // Fallthru to tail compare
  }
  // Shift str2 and str1 to the end of the arrays, negate min
  if (ae == StrIntrinsicNode::LL || ae == StrIntrinsicNode::UU) {
    lea(str1, Address(str1, cnt2, scale));
    lea(str2, Address(str2, cnt2, scale));
  } else {
    lea(str1, Address(str1, cnt2, scale1));
    lea(str2, Address(str2, cnt2, scale2));
  }
  decrementl(cnt2);  // first character was compared already
  negptr(cnt2);

  // Compare the rest of the elements
  bind(WHILE_HEAD_LABEL);
  load_next_elements(result, cnt1, str1, str2, scale, scale1, scale2, cnt2, ae);
  subl(result, cnt1);
  jccb(Assembler::notZero, POP_LABEL);
  increment(cnt2);
  jccb(Assembler::notZero, WHILE_HEAD_LABEL);

  // Strings are equal up to min length.  Return the length difference.
  bind(LENGTH_DIFF_LABEL);
  pop(result);
  if (ae == StrIntrinsicNode::UU) {
    // Divide diff by 2 to get number of chars
    sarl(result, 1);
  }
  jmpb(DONE_LABEL);

#ifdef _LP64
  if (VM_Version::supports_avx512vlbw()) {

    bind(COMPARE_WIDE_VECTORS_LOOP_FAILED);

    kmovql(cnt1, k7);
    notq(cnt1);
    bsfq(cnt2, cnt1);
    if (ae != StrIntrinsicNode::LL) {
      // Divide diff by 2 to get number of chars
      sarl(cnt2, 1);
    }
    addq(result, cnt2);
    if (ae == StrIntrinsicNode::LL) {
      load_unsigned_byte(cnt1, Address(str2, result));
      load_unsigned_byte(result, Address(str1, result));
    } else if (ae == StrIntrinsicNode::UU) {
      load_unsigned_short(cnt1, Address(str2, result, scale));
      load_unsigned_short(result, Address(str1, result, scale));
    } else {
      load_unsigned_short(cnt1, Address(str2, result, scale2));
      load_unsigned_byte(result, Address(str1, result, scale1));
    }
    subl(result, cnt1);
    jmpb(POP_LABEL);
  }//if (VM_Version::supports_avx512vlbw())
#endif // _LP64

  // Discard the stored length difference
  bind(POP_LABEL);
  pop(cnt1);

  // That's it
  bind(DONE_LABEL);
  if(ae == StrIntrinsicNode::UL) {
    negl(result);
  }

}

// Search for Non-ASCII character (Negative byte value) in a byte array,
// return true if it has any and false otherwise.
//   ..\jdk\src\java.base\share\classes\java\lang\StringCoding.java
//   @HotSpotIntrinsicCandidate
//   private static boolean hasNegatives(byte[] ba, int off, int len) {
//     for (int i = off; i < off + len; i++) {
//       if (ba[i] < 0) {
//         return true;
//       }
//     }
//     return false;
//   }
void C2_MacroAssembler::has_negatives(Register ary1, Register len,
  Register result, Register tmp1,
  XMMRegister vec1, XMMRegister vec2) {
  // rsi: byte array
  // rcx: len
  // rax: result
  ShortBranchVerifier sbv(this);
  assert_different_registers(ary1, len, result, tmp1);
  assert_different_registers(vec1, vec2);
  Label TRUE_LABEL, FALSE_LABEL, DONE, COMPARE_CHAR, COMPARE_VECTORS, COMPARE_BYTE;

  // len == 0
  testl(len, len);
  jcc(Assembler::zero, FALSE_LABEL);

  if ((AVX3Threshold == 0) && (UseAVX > 2) && // AVX512
    VM_Version::supports_avx512vlbw() &&
    VM_Version::supports_bmi2()) {

    Label test_64_loop, test_tail;
    Register tmp3_aliased = len;

    movl(tmp1, len);
    vpxor(vec2, vec2, vec2, Assembler::AVX_512bit);

    andl(tmp1, 64 - 1);   // tail count (in chars) 0x3F
    andl(len, ~(64 - 1));    // vector count (in chars)
    jccb(Assembler::zero, test_tail);

    lea(ary1, Address(ary1, len, Address::times_1));
    negptr(len);

    bind(test_64_loop);
    // Check whether our 64 elements of size byte contain negatives
    evpcmpgtb(k2, vec2, Address(ary1, len, Address::times_1), Assembler::AVX_512bit);
    kortestql(k2, k2);
    jcc(Assembler::notZero, TRUE_LABEL);

    addptr(len, 64);
    jccb(Assembler::notZero, test_64_loop);


    bind(test_tail);
    // bail out when there is nothing to be done
    testl(tmp1, -1);
    jcc(Assembler::zero, FALSE_LABEL);

    // ~(~0 << len) applied up to two times (for 32-bit scenario)
#ifdef _LP64
    mov64(tmp3_aliased, 0xFFFFFFFFFFFFFFFF);
    shlxq(tmp3_aliased, tmp3_aliased, tmp1);
    notq(tmp3_aliased);
    kmovql(k3, tmp3_aliased);
#else
    Label k_init;
    jmp(k_init);

    // We could not read 64-bits from a general purpose register thus we move
    // data required to compose 64 1's to the instruction stream
    // We emit 64 byte wide series of elements from 0..63 which later on would
    // be used as a compare targets with tail count contained in tmp1 register.
    // Result would be a k register having tmp1 consecutive number or 1
    // counting from least significant bit.
    address tmp = pc();
    emit_int64(0x0706050403020100);
    emit_int64(0x0F0E0D0C0B0A0908);
    emit_int64(0x1716151413121110);
    emit_int64(0x1F1E1D1C1B1A1918);
    emit_int64(0x2726252423222120);
    emit_int64(0x2F2E2D2C2B2A2928);
    emit_int64(0x3736353433323130);
    emit_int64(0x3F3E3D3C3B3A3938);

    bind(k_init);
    lea(len, InternalAddress(tmp));
    // create mask to test for negative byte inside a vector
    evpbroadcastb(vec1, tmp1, Assembler::AVX_512bit);
    evpcmpgtb(k3, vec1, Address(len, 0), Assembler::AVX_512bit);

#endif
    evpcmpgtb(k2, k3, vec2, Address(ary1, 0), Assembler::AVX_512bit);
    ktestq(k2, k3);
    jcc(Assembler::notZero, TRUE_LABEL);

    jmp(FALSE_LABEL);
  } else {
    movl(result, len); // copy

    if (UseAVX >= 2 && UseSSE >= 2) {
      // With AVX2, use 32-byte vector compare
      Label COMPARE_WIDE_VECTORS, COMPARE_TAIL;

      // Compare 32-byte vectors
      andl(result, 0x0000001f);  //   tail count (in bytes)
      andl(len, 0xffffffe0);   // vector count (in bytes)
      jccb(Assembler::zero, COMPARE_TAIL);

      lea(ary1, Address(ary1, len, Address::times_1));
      negptr(len);

      movl(tmp1, 0x80808080);   // create mask to test for Unicode chars in vector
      movdl(vec2, tmp1);
      vpbroadcastd(vec2, vec2, Assembler::AVX_256bit);

      bind(COMPARE_WIDE_VECTORS);
      vmovdqu(vec1, Address(ary1, len, Address::times_1));
      vptest(vec1, vec2);
      jccb(Assembler::notZero, TRUE_LABEL);
      addptr(len, 32);
      jcc(Assembler::notZero, COMPARE_WIDE_VECTORS);

      testl(result, result);
      jccb(Assembler::zero, FALSE_LABEL);

      vmovdqu(vec1, Address(ary1, result, Address::times_1, -32));
      vptest(vec1, vec2);
      jccb(Assembler::notZero, TRUE_LABEL);
      jmpb(FALSE_LABEL);

      bind(COMPARE_TAIL); // len is zero
      movl(len, result);
      // Fallthru to tail compare
    } else if (UseSSE42Intrinsics) {
      // With SSE4.2, use double quad vector compare
      Label COMPARE_WIDE_VECTORS, COMPARE_TAIL;

      // Compare 16-byte vectors
      andl(result, 0x0000000f);  //   tail count (in bytes)
      andl(len, 0xfffffff0);   // vector count (in bytes)
      jcc(Assembler::zero, COMPARE_TAIL);

      lea(ary1, Address(ary1, len, Address::times_1));
      negptr(len);

      movl(tmp1, 0x80808080);
      movdl(vec2, tmp1);
      pshufd(vec2, vec2, 0);

      bind(COMPARE_WIDE_VECTORS);
      movdqu(vec1, Address(ary1, len, Address::times_1));
      ptest(vec1, vec2);
      jcc(Assembler::notZero, TRUE_LABEL);
      addptr(len, 16);
      jcc(Assembler::notZero, COMPARE_WIDE_VECTORS);

      testl(result, result);
      jcc(Assembler::zero, FALSE_LABEL);

      movdqu(vec1, Address(ary1, result, Address::times_1, -16));
      ptest(vec1, vec2);
      jccb(Assembler::notZero, TRUE_LABEL);
      jmpb(FALSE_LABEL);

      bind(COMPARE_TAIL); // len is zero
      movl(len, result);
      // Fallthru to tail compare
    }
  }
  // Compare 4-byte vectors
  andl(len, 0xfffffffc); // vector count (in bytes)
  jccb(Assembler::zero, COMPARE_CHAR);

  lea(ary1, Address(ary1, len, Address::times_1));
  negptr(len);

  bind(COMPARE_VECTORS);
  movl(tmp1, Address(ary1, len, Address::times_1));
  andl(tmp1, 0x80808080);
  jccb(Assembler::notZero, TRUE_LABEL);
  addptr(len, 4);
  jcc(Assembler::notZero, COMPARE_VECTORS);

  // Compare trailing char (final 2 bytes), if any
  bind(COMPARE_CHAR);
  testl(result, 0x2);   // tail  char
  jccb(Assembler::zero, COMPARE_BYTE);
  load_unsigned_short(tmp1, Address(ary1, 0));
  andl(tmp1, 0x00008080);
  jccb(Assembler::notZero, TRUE_LABEL);
  subptr(result, 2);
  lea(ary1, Address(ary1, 2));

  bind(COMPARE_BYTE);
  testl(result, 0x1);   // tail  byte
  jccb(Assembler::zero, FALSE_LABEL);
  load_unsigned_byte(tmp1, Address(ary1, 0));
  andl(tmp1, 0x00000080);
  jccb(Assembler::notEqual, TRUE_LABEL);
  jmpb(FALSE_LABEL);

  bind(TRUE_LABEL);
  movl(result, 1);   // return true
  jmpb(DONE);

  bind(FALSE_LABEL);
  xorl(result, result); // return false

  // That's it
  bind(DONE);
  if (UseAVX >= 2 && UseSSE >= 2) {
    // clean upper bits of YMM registers
    vpxor(vec1, vec1);
    vpxor(vec2, vec2);
  }
}
// Compare char[] or byte[] arrays aligned to 4 bytes or substrings.
void C2_MacroAssembler::arrays_equals(bool is_array_equ, Register ary1, Register ary2,
                                      Register limit, Register result, Register chr,
                                      XMMRegister vec1, XMMRegister vec2, bool is_char) {
  ShortBranchVerifier sbv(this);
  Label TRUE_LABEL, FALSE_LABEL, DONE, COMPARE_VECTORS, COMPARE_CHAR, COMPARE_BYTE;

  int length_offset  = arrayOopDesc::length_offset_in_bytes();
  int base_offset    = arrayOopDesc::base_offset_in_bytes(is_char ? T_CHAR : T_BYTE);

  if (is_array_equ) {
    // Check the input args
    cmpoop(ary1, ary2);
    jcc(Assembler::equal, TRUE_LABEL);

    // Need additional checks for arrays_equals.
    testptr(ary1, ary1);
    jcc(Assembler::zero, FALSE_LABEL);
    testptr(ary2, ary2);
    jcc(Assembler::zero, FALSE_LABEL);

    // Check the lengths
    movl(limit, Address(ary1, length_offset));
    cmpl(limit, Address(ary2, length_offset));
    jcc(Assembler::notEqual, FALSE_LABEL);
  }

  // count == 0
  testl(limit, limit);
  jcc(Assembler::zero, TRUE_LABEL);

  if (is_array_equ) {
    // Load array address
    lea(ary1, Address(ary1, base_offset));
    lea(ary2, Address(ary2, base_offset));
  }

  if (is_array_equ && is_char) {
    // arrays_equals when used for char[].
    shll(limit, 1);      // byte count != 0
  }
  movl(result, limit); // copy

  if (UseAVX >= 2) {
    // With AVX2, use 32-byte vector compare
    Label COMPARE_WIDE_VECTORS, COMPARE_TAIL;

    // Compare 32-byte vectors
    andl(result, 0x0000001f);  //   tail count (in bytes)
    andl(limit, 0xffffffe0);   // vector count (in bytes)
    jcc(Assembler::zero, COMPARE_TAIL);

    lea(ary1, Address(ary1, limit, Address::times_1));
    lea(ary2, Address(ary2, limit, Address::times_1));
    negptr(limit);

#ifdef _LP64
    if ((AVX3Threshold == 0) && VM_Version::supports_avx512vlbw()) { // trying 64 bytes fast loop
      Label COMPARE_WIDE_VECTORS_LOOP_AVX2, COMPARE_WIDE_VECTORS_LOOP_AVX3;

      cmpl(limit, -64);
      jcc(Assembler::greater, COMPARE_WIDE_VECTORS_LOOP_AVX2);

      bind(COMPARE_WIDE_VECTORS_LOOP_AVX3); // the hottest loop

      evmovdquq(vec1, Address(ary1, limit, Address::times_1), Assembler::AVX_512bit);
      evpcmpeqb(k7, vec1, Address(ary2, limit, Address::times_1), Assembler::AVX_512bit);
      kortestql(k7, k7);
      jcc(Assembler::aboveEqual, FALSE_LABEL);     // miscompare
      addptr(limit, 64);  // update since we already compared at this addr
      cmpl(limit, -64);
      jccb(Assembler::lessEqual, COMPARE_WIDE_VECTORS_LOOP_AVX3);

      // At this point we may still need to compare -limit+result bytes.
      // We could execute the next two instruction and just continue via non-wide path:
      //  cmpl(limit, 0);
      //  jcc(Assembler::equal, COMPARE_TAIL);  // true
      // But since we stopped at the points ary{1,2}+limit which are
      // not farther than 64 bytes from the ends of arrays ary{1,2}+result
      // (|limit| <= 32 and result < 32),
      // we may just compare the last 64 bytes.
      //
      addptr(result, -64);   // it is safe, bc we just came from this area
      evmovdquq(vec1, Address(ary1, result, Address::times_1), Assembler::AVX_512bit);
      evpcmpeqb(k7, vec1, Address(ary2, result, Address::times_1), Assembler::AVX_512bit);
      kortestql(k7, k7);
      jcc(Assembler::aboveEqual, FALSE_LABEL);     // miscompare

      jmp(TRUE_LABEL);

      bind(COMPARE_WIDE_VECTORS_LOOP_AVX2);

    }//if (VM_Version::supports_avx512vlbw())
#endif //_LP64
    bind(COMPARE_WIDE_VECTORS);
    vmovdqu(vec1, Address(ary1, limit, Address::times_1));
    vmovdqu(vec2, Address(ary2, limit, Address::times_1));
    vpxor(vec1, vec2);

    vptest(vec1, vec1);
    jcc(Assembler::notZero, FALSE_LABEL);
    addptr(limit, 32);
    jcc(Assembler::notZero, COMPARE_WIDE_VECTORS);

    testl(result, result);
    jcc(Assembler::zero, TRUE_LABEL);

    vmovdqu(vec1, Address(ary1, result, Address::times_1, -32));
    vmovdqu(vec2, Address(ary2, result, Address::times_1, -32));
    vpxor(vec1, vec2);

    vptest(vec1, vec1);
    jccb(Assembler::notZero, FALSE_LABEL);
    jmpb(TRUE_LABEL);

    bind(COMPARE_TAIL); // limit is zero
    movl(limit, result);
    // Fallthru to tail compare
  } else if (UseSSE42Intrinsics) {
    // With SSE4.2, use double quad vector compare
    Label COMPARE_WIDE_VECTORS, COMPARE_TAIL;

    // Compare 16-byte vectors
    andl(result, 0x0000000f);  //   tail count (in bytes)
    andl(limit, 0xfffffff0);   // vector count (in bytes)
    jcc(Assembler::zero, COMPARE_TAIL);

    lea(ary1, Address(ary1, limit, Address::times_1));
    lea(ary2, Address(ary2, limit, Address::times_1));
    negptr(limit);

    bind(COMPARE_WIDE_VECTORS);
    movdqu(vec1, Address(ary1, limit, Address::times_1));
    movdqu(vec2, Address(ary2, limit, Address::times_1));
    pxor(vec1, vec2);

    ptest(vec1, vec1);
    jcc(Assembler::notZero, FALSE_LABEL);
    addptr(limit, 16);
    jcc(Assembler::notZero, COMPARE_WIDE_VECTORS);

    testl(result, result);
    jcc(Assembler::zero, TRUE_LABEL);

    movdqu(vec1, Address(ary1, result, Address::times_1, -16));
    movdqu(vec2, Address(ary2, result, Address::times_1, -16));
    pxor(vec1, vec2);

    ptest(vec1, vec1);
    jccb(Assembler::notZero, FALSE_LABEL);
    jmpb(TRUE_LABEL);

    bind(COMPARE_TAIL); // limit is zero
    movl(limit, result);
    // Fallthru to tail compare
  }

  // Compare 4-byte vectors
  andl(limit, 0xfffffffc); // vector count (in bytes)
  jccb(Assembler::zero, COMPARE_CHAR);

  lea(ary1, Address(ary1, limit, Address::times_1));
  lea(ary2, Address(ary2, limit, Address::times_1));
  negptr(limit);

  bind(COMPARE_VECTORS);
  movl(chr, Address(ary1, limit, Address::times_1));
  cmpl(chr, Address(ary2, limit, Address::times_1));
  jccb(Assembler::notEqual, FALSE_LABEL);
  addptr(limit, 4);
  jcc(Assembler::notZero, COMPARE_VECTORS);

  // Compare trailing char (final 2 bytes), if any
  bind(COMPARE_CHAR);
  testl(result, 0x2);   // tail  char
  jccb(Assembler::zero, COMPARE_BYTE);
  load_unsigned_short(chr, Address(ary1, 0));
  load_unsigned_short(limit, Address(ary2, 0));
  cmpl(chr, limit);
  jccb(Assembler::notEqual, FALSE_LABEL);

  if (is_array_equ && is_char) {
    bind(COMPARE_BYTE);
  } else {
    lea(ary1, Address(ary1, 2));
    lea(ary2, Address(ary2, 2));

    bind(COMPARE_BYTE);
    testl(result, 0x1);   // tail  byte
    jccb(Assembler::zero, TRUE_LABEL);
    load_unsigned_byte(chr, Address(ary1, 0));
    load_unsigned_byte(limit, Address(ary2, 0));
    cmpl(chr, limit);
    jccb(Assembler::notEqual, FALSE_LABEL);
  }
  bind(TRUE_LABEL);
  movl(result, 1);   // return true
  jmpb(DONE);

  bind(FALSE_LABEL);
  xorl(result, result); // return false

  // That's it
  bind(DONE);
  if (UseAVX >= 2) {
    // clean upper bits of YMM registers
    vpxor(vec1, vec1);
    vpxor(vec2, vec2);
  }
}
