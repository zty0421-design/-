(function(){
  'use strict';

  class NativeSocket {
    constructor(options){
      this.options=options||{};
      this.handlers=new Map();
      this.queue=[];
      this.socket=null;
      this.closedByUser=false;
      this.retry=0;
      this.retryTimer=null;
      this.opening=false;
      this.connected=false;
      this.onlineHandler=()=>this.reconnect();
      this.offlineHandler=()=>this.dispatch('disconnect',{code:0,reason:'offline'});
      window.addEventListener('online',this.onlineHandler);
      window.addEventListener('offline',this.offlineHandler);
      this.open();
    }

    on(event,handler){
      if(typeof handler!=='function')return this;
      if(!this.handlers.has(event))this.handlers.set(event,new Set());
      this.handlers.get(event).add(handler);
      return this;
    }

    dispatch(event,data){
      for(const handler of this.handlers.get(event)||[]){
        try{handler(data);}catch(error){console.error('WebSocket handler failed',error);}
      }
    }

    async createTicket(){
      const token=String(this.options?.auth?.token||'');
      if(!token)return '';
      try{
        const controller=new AbortController();const timer=setTimeout(()=>controller.abort(),8000);
        const response=await fetch('/api/realtime-ticket',{
          method:'POST',headers:{'Authorization':`Bearer ${token}`,'Content-Type':'application/json'},body:'{}',cache:'no-store',signal:controller.signal
        }).finally(()=>clearTimeout(timer));
        if(!response.ok)return '';
        const data=await response.json().catch(()=>({}));
        return String(data.ticket||'');
      }catch(error){
        console.warn('建立即時連線票證失敗，嘗試相容模式',error);return '';
      }
    }

    async open(){
      if(this.closedByUser||this.opening||!navigator.onLine)return;
      if(this.socket&&(this.socket.readyState===WebSocket.OPEN||this.socket.readyState===WebSocket.CONNECTING))return;
      this.opening=true;
      try{
        const scheme=location.protocol==='https:'?'wss:':'ws:';
        const token=String(this.options?.auth?.token||'');
        const ticket=await this.createTicket();
        if(this.closedByUser)return;
        const authQuery=ticket?`ticket=${encodeURIComponent(ticket)}`:`token=${encodeURIComponent(token)}`;
        const url=`${scheme}//${location.host}/ws?${authQuery}`;
        let socket;
        try{socket=new WebSocket(url);}catch(error){this.scheduleReconnect();return;}
        this.socket=socket;
        socket.addEventListener('open',()=>{
          this.connected=true;this.retry=0;this.dispatch('connect',{queued:this.queue.length});
          const queued=this.queue.splice(0);for(const packet of queued)this.sendPacket(packet);
        });
        socket.addEventListener('message',event=>{
          try{const packet=JSON.parse(event.data);if(packet?.event&&packet.event!=='connect')this.dispatch(packet.event,packet.data);}
          catch(error){console.warn('忽略無法解析的即時訊息',error);}
        });
        socket.addEventListener('close',event=>{
          if(this.socket!==socket)return;
          this.connected=false;
          if(!this.closedByUser){
            this.dispatch('disconnect',{code:event.code,reason:event.reason||''});
            this.dispatch('connect_error',{code:event.code,reason:event.reason||''});
            this.scheduleReconnect();
          }
        });
        socket.addEventListener('error',()=>{});
      }finally{this.opening=false;}
    }

    sendPacket(packet){
      if(this.socket?.readyState===WebSocket.OPEN){this.socket.send(JSON.stringify(packet));return true;}
      return false;
    }

    emit(event,data){
      const packet={event,data:data??{}};
      if(!this.sendPacket(packet)){if(this.queue.length>=50)this.queue.shift();this.queue.push(packet);}
      return this;
    }

    scheduleReconnect(){
      if(this.closedByUser||this.retryTimer||!navigator.onLine)return;
      const delay=Math.min(15000,750*(2**Math.min(this.retry,5)));
      this.retry+=1;
      this.dispatch('reconnecting',{attempt:this.retry,delay});
      this.retryTimer=setTimeout(()=>{this.retryTimer=null;this.open();},delay);
    }

    reconnect(){
      if(this.closedByUser||!navigator.onLine)return this;
      clearTimeout(this.retryTimer);this.retryTimer=null;
      const old=this.socket;this.socket=null;this.connected=false;
      if(old&&old.readyState<2){try{old.close(4000,'reconnect');}catch(_){}}
      this.open();return this;
    }

    disconnect(){
      this.closedByUser=true;this.connected=false;clearTimeout(this.retryTimer);this.retryTimer=null;this.queue=[];
      window.removeEventListener('online',this.onlineHandler);window.removeEventListener('offline',this.offlineHandler);
      if(this.socket&&this.socket.readyState<2)this.socket.close(1000,'client disconnect');
    }
  }

  window.io=options=>new NativeSocket(options);
})();
