def kill_processes(processes):
    plist = [p for p in processes if p is not None]

    for p in plist:
        p.terminate()
    for p in plist:
        try:
            p.wait(timeout=0.5)
        except:
            pass
    for p in plist:
        p.kill()
    for p in plist:
        p.wait(timeout=1.0)