"""Local UI harness only. Does not open serial ports or access hardware."""
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path
import json,time,threading,sys
state={'power':1,'mode':0,'cadr':14,'water':40,'humidity':5320,'temperature':2735,'hThreshold':50,'waterflood':0,'uvlamp':1,'lock':0,'anion':0}
data={'token':'ui-test-only','connected':True,'armed':False,'pending':False,'configReady':True,'state':state,'result':'界面测试数据 · 未连接实物','commandId':0,'rxFrames':3,'invalidFrames':0,'ageMs':100,'logs':[]}
data.update(version='1.0.0',otaReady=False,otaBusy=False,network={'configured':False,'connected':False,'canConfigure':True,'ssid':'','ip':'','message':'尚未配置家庭 Wi-Fi'})
lock=threading.Lock()
class Handler(BaseHTTPRequestHandler):
 def reply(self,obj,code=200):
  b=json.dumps(obj,ensure_ascii=False).encode();self.send_response(code);self.send_header('Content-Type','application/json');self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(b)
 def do_GET(self):
  if self.path=='/api/status':
   with lock:self.reply(data)
  elif self.path=='/':
   b=(Path(__file__).resolve().parents[1]/'web/index.html').read_bytes();self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.end_headers();self.wfile.write(b)
  else:self.reply({},404)
 def do_POST(self):
  obj=json.loads(self.rfile.read(int(self.headers.get('Content-Length','0'))) or b'{}')
  with lock:
   if self.path=='/test/scenario':data['connected']=obj.get('connected',True);data['armed']=False;self.reply({'ok':True});return
   if self.headers.get('X-A3-Token')!=data['token']:self.reply({'error':'bad token'},403);return
   if self.path=='/api/network':
    if len(obj.get('ssid','').encode()) not in range(1,33) or len(obj.get('password','').encode()) not in range(8,64):self.reply({'error':'bad credentials'},400);return
    data['network'].update(configured=True,connected=True,ssid=obj['ssid'],ip='192.168.1.88',message='模拟：已连接家庭 Wi-Fi');self.reply({'ok':True});return
   if self.path=='/api/arm':data['armed']=True;data['result']='本次测试已启用，有效 60 秒';self.reply({'ok':True});return
   if self.path!='/api/control':self.reply({},404);return
   if not data['armed'] or data['pending']:self.reply({'error':'请重新启用'},409);return
   data['pending']=True;data['armed']=False;data['commandId']+=1;data['result']='已发送，等待应答与状态';self.reply({'sent':True})
   def finish():
    with lock:
     a,v=obj['action'],obj['value']
     if a=='speed':state['mode']=0;state['cadr']=[14,29,64,85,100][v-1]
     elif a=='airflow':state['mode']=0;state['cadr']=v
     elif a=='mode':state['mode']=v
     elif a=='refill':state['waterflood']=v;state['cadr']=0 if v else 14
     elif a=='power':state['power']=v
     data['pending']=False;data['result']='收到应答，状态已匹配（仅界面测试）'
   threading.Timer(2,finish).start()
 def log_message(self,*args):pass
ThreadingHTTPServer(('127.0.0.1',int(sys.argv[1]) if len(sys.argv)>1 else 8765),Handler).serve_forever()
