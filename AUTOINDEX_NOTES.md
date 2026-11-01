| Function | File | Role | Our change |
|---|---|---|---|
| ExecInitSeqScan | executor/nodeSeqscan.c | builds SeqScanState from SeqScan plan | detect candidate |
| SeqNext | executor/nodeSeqscan.c | fetches next heap tuple | count examined, detect end |
| ExecSeqScanWithQual | executor/nodeSeqscan.c | loop: fetch + evaluate qual | count matched |
| ExecEndSeqScan | executor/nodeSeqscan.c | cleanup | report to shared memory |
| cost_seqscan | optimizer/path/costsize.c | planner cost of seq scan | (read only, we mirror its formula) |
| DefineIndex | commands/indexcmds.c | implements CREATE INDEX | (called via SPI by worker) |
| RegisterBackgroundWorker | postmaster/bgworker.c | starts worker processes | register our worker |
| CreateOrAttachShmemStructs | storage/ipc/ipci.c | allocates shared memory at startup | allocate our table |