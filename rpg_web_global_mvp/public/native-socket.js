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
        const response=await fetch('/api/realtime-ticket',{
          method:'POST',
          headers:{'Authorization':`Bearer ${token}`,'Content-Type':'application/json'},
          body:'{}',
          cache:'no-store'
        });
        if(!response.ok)return '';
        const data=await response.json().catch(()=>({}));
        return String(data.ticket||'');
      }catch(error){
        console.warn('建立即時連線票證失敗，嘗試相容模式',error);
        return '';
      }
    }

    async open(){
      if(this.closedByUser||this.opening)return;
      this.opening=true;
      try{
        const scheme=location.protocol==='https:'?'wss:':'ws:';
        const token=String(this.options?.auth?.token||'');
        const ticket=await this.createTicket();
        if(this.closedByUser)return;
        // 58-cpp.19：優先使用短效一次性 ticket；舊伺服器才退回 JWT query。
        const authQuery=ticket?`ticket=${encodeURIComponent(ticket)}`:`token=${encodeURIComponent(token)}`;
        const url=`${scheme}//${location.host}/ws?${authQuery}`;
        let socket;
        try{socket=new WebSocket(url);}catch(error){this.scheduleReconnect();return;}
        this.socket=socket;
        socket.addEventListener('open',()=>{
          this.retry=0;
          this.dispatch('connect');
          const queued=this.queue.splice(0);
          for(const packet of queued)this.sendPacket(packet);
        });
        socket.addEventListener('message',event=>{
          try{
            const packet=JSON.parse(event.data);
            if(packet?.event&&packet.event!=='connect')this.dispatch(packet.event,packet.data);
          }catch(error){console.warn('忽略無法解析的即時訊息',error);}
        });
        socket.addEventListener('close',event=>{
          if(!this.closedByUser){
            this.dispatch('connect_error',{code:event.code,reason:event.reason||''});
            this.scheduleReconnect();
          }
        });
        socket.addEventListener('error',()=>{});
      }finally{
        this.opening=false;
      }
    }

    sendPacket(packet){
      if(this.socket?.readyState===WebSocket.OPEN){
        this.socket.send(JSON.stringify(packet));
        return true;
      }
      return false;
    }

    emit(event,data){
      const packet={event,data:data??{}};
      if(!this.sendPacket(packet)){
        if(this.queue.length>=50)this.queue.shift();
        this.queue.push(packet);
      }
      return this;
    }

    scheduleReconnect(){
      if(this.closedByUser||this.retryTimer)return;
      const delay=Math.min(15000,750*(2**Math.min(this.retry,5)));
      this.retry+=1;
      this.retryTimer=setTimeout(()=>{this.retryTimer=null;this.open();},delay);
    }

    disconnect(){
      this.closedByUser=true;
      clearTimeout(this.retryTimer);
      this.retryTimer=null;
      this.queue=[];
      if(this.socket&&this.socket.readyState<2)this.socket.close(1000,'client disconnect');
    }
  }

  window.io=options=>new NativeSocket(options);
})();
