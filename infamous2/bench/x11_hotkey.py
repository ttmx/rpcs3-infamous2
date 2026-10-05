"""Local X11 activation/hotkey helper. No XTest, focus forcing, or portal APIs.
Default target guard: inFamous 2. Merely importing has no UI side effects.
"""
import argparse
import ctypes as c
import json
import time

class Key(c.Structure):
    _fields_=[('type',c.c_int),('serial',c.c_ulong),('send_event',c.c_int),('display',c.c_void_p),('window',c.c_ulong),('root',c.c_ulong),('subwindow',c.c_ulong),('time',c.c_ulong),('x',c.c_int),('y',c.c_int),('x_root',c.c_int),('y_root',c.c_int),('state',c.c_uint),('keycode',c.c_uint),('same_screen',c.c_int)]
class ClientData(c.Union):
    _fields_=[('b',c.c_char*20),('s',c.c_short*10),('l',c.c_long*5)]
class Client(c.Structure):
    _fields_=[('type',c.c_int),('serial',c.c_ulong),('send_event',c.c_int),('display',c.c_void_p),('window',c.c_ulong),('message_type',c.c_ulong),('format',c.c_int),('data',ClientData)]
class Event(c.Union):
    _fields_=[('key',Key),('client',Client),('pad',c.c_long*24)]

class Connection:
    def __init__(self):
        self.x=c.CDLL('libX11.so.6')
        specs={'XOpenDisplay':([c.c_char_p],c.c_void_p),'XDefaultRootWindow':([c.c_void_p],c.c_ulong),'XInternAtom':([c.c_void_p,c.c_char_p,c.c_int],c.c_ulong),'XStringToKeysym':([c.c_char_p],c.c_ulong),'XKeysymToKeycode':([c.c_void_p,c.c_ulong],c.c_uint),'XFetchName':([c.c_void_p,c.c_ulong,c.POINTER(c.c_void_p)],c.c_int),'XSendEvent':([c.c_void_p,c.c_ulong,c.c_int,c.c_long,c.POINTER(Event)],c.c_int),'XFlush':([c.c_void_p],c.c_int),'XCloseDisplay':([c.c_void_p],c.c_int),'XFree':([c.c_void_p],c.c_int),'XGetWindowProperty':([c.c_void_p,c.c_ulong,c.c_ulong,c.c_long,c.c_long,c.c_int,c.c_ulong,c.POINTER(c.c_ulong),c.POINTER(c.c_int),c.POINTER(c.c_ulong),c.POINTER(c.c_ulong),c.POINTER(c.c_void_p)],c.c_int)}
        for name,(args,res) in specs.items():
            fun=getattr(self.x,name);fun.argtypes=args;fun.restype=res
        self.d=self.x.XOpenDisplay(None)
        if not self.d: raise RuntimeError('Cannot connect to X11 display')
        self.root=self.x.XDefaultRootWindow(self.d)
    def atom(self,name): return self.x.XInternAtom(self.d,name.encode(),0)
    def property(self,w,name):
        typ=c.c_ulong();fmt=c.c_int();n=c.c_ulong();left=c.c_ulong();data=c.c_void_p()
        rc=self.x.XGetWindowProperty(self.d,w,self.atom(name),0,16,0,0,c.byref(typ),c.byref(fmt),c.byref(n),c.byref(left),c.byref(data))
        try:
            if rc or not data.value or fmt.value!=32:return []
            return list(c.cast(data,c.POINTER(c.c_ulong))[:n.value])
        finally:
            if data.value:self.x.XFree(data)
    def title(self,w):
        data=c.c_void_p();self.x.XFetchName(self.d,w,c.byref(data))
        try:return c.string_at(data).decode(errors='replace') if data.value else ''
        finally:
            if data.value:self.x.XFree(data)
    def activate(self,w,timeout=0.8):
        old=self.property(self.root,'_NET_ACTIVE_WINDOW')
        stamp=self.property(w,'_NET_WM_USER_TIME')
        e=Event();e.client=Client(type=33,display=self.d,window=w,message_type=self.atom('_NET_ACTIVE_WINDOW'),format=32)
        e.client.data.l[0]=2 # EWMH pager source
        e.client.data.l[1]=stamp[0] if stamp else 0
        e.client.data.l[2]=old[0] if old else 0
        self.x.XSendEvent(self.d,self.root,0,(1<<20)|(1<<19),c.byref(e));self.x.XFlush(self.d)
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            active=self.property(self.root,'_NET_ACTIVE_WINDOW')
            if active and active[0]==w:return True
            time.sleep(0.025)
        return False
    def send(self,w,key,state=0,hold=0.12):
        code=self.x.XKeysymToKeycode(self.d,self.x.XStringToKeysym(key.encode()))
        if not code:raise ValueError('Unknown key '+key)
        for typ in [2,3]:
            e=Event();e.key=Key(type=typ,display=self.d,window=w,root=self.root,time=0,x=1,y=1,same_screen=1,state=state,keycode=code)
            self.x.XSendEvent(self.d,w,0,1<<(typ-2),c.byref(e));self.x.XFlush(self.d)
            if typ==2:time.sleep(hold)
    def close(self):self.x.XCloseDisplay(self.d)

def main():
    p=argparse.ArgumentParser();p.add_argument('window',type=lambda s:int(s,0));p.add_argument('key',nargs='?',default=None);p.add_argument('--state',type=lambda s:int(s,0),default=0);p.add_argument('--activate',action='store_true');p.add_argument('--expected-title',default='inFamous 2');p.add_argument('--hold',type=float,default=.12);p.add_argument('--timeout',type=float,default=.8)
    a=p.parse_args();conn=Connection()
    try:
        title=conn.title(a.window)
        if a.expected_title not in title:raise SystemExit('Refusing unexpected window: '+repr(title))
        activated=conn.activate(a.window,a.timeout) if a.activate else None
        print(json.dumps({'window':hex(a.window),'title':title,'activated':activated}),flush=True)
        if a.activate and not activated:raise SystemExit('Window manager did not activate target; hotkey not sent')
        if a.key:conn.send(a.window,a.key,a.state,a.hold)
    finally:conn.close()
if __name__=='__main__':main()
