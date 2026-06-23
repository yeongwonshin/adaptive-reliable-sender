MP1 complete sender package

1) What to submit
- The assignment requires submitting exactly one source file:
  sender_<your_student_id>.cc
- Do NOT submit netsim, netsim.h, netsim_lib.cc, input files, output files, or validation logs.
- This package uses sender_20200000.cc as a placeholder filename. Rename it before submission.

2) Grading compile command
The grader will compile like this:
  g++ -O2 -o sender_<your_student_id> sender_<your_student_id>.cc netsim_lib.cc

3) Local compile example
  g++ -O2 -o sender sender.cc netsim_lib.cc

4) Local validation examples
  ./netsim ./sender --input sherlock_holmes.txt --output out.rx --ber 1e-6 --seed 1001 --max_bytes 386832300
  diff sherlock_holmes.txt out.rx

  ./netsim ./sender --input cat_bgm.mp3 --output out.rx --ber 1e-5 --seed 2002 --max_bytes 316224000
  diff cat_bgm.mp3 out.rx

  ./netsim ./sender --input cat_bgm.mp3 --output out.rx --ber 1e-4 --seed 3003 --max_bytes 316224000
  diff cat_bgm.mp3 out.rx

  ./netsim ./sender --input harry_potter.txt --output out.rx --ber 1e-3 --seed 4004 --max_bytes 44276800
  diff harry_potter.txt out.rx

5) Verification done in this environment
- The source compiled successfully using the grading-style command.
- All four public validation scenarios ended with status: SUCCESS.
- cmp/diff matched the original input in all four scenarios.
- Detailed logs are in validation_logs/.
