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
      this.connectTimer=null;
      this.heartbeatTimer=null;
      this.lastActivityAt=0;
      this.lastPingAt=0;
      this.pingSeq=0;
      this.ticketFallbackUntil=0;
      this.openGeneration=0;
      this.onlineHandler=()=>this.reconnect();
      this.offlineHandler=()=>this.dispatch('disconnect',{code:0,reason:'offline'});
      this.visibilityHandler=()=>{
        if(document.visibilityState==='visible'&&!this.closedByUser){
          const stale=!this.connected||(Date.now()-this.lastActivityAt>45000);
          if(stale)this.reconnect();
        }
      };
      window.addEventListener('online',this.onlineHandler);
      window.addEventListener('offline',this.offlineHandler);
      document.addEventListener('visibilitychange',this.visibilityHandler);
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
      // 票證端點若暫時失敗，不要讓每一次 WebSocket 重連都先等一次 HTTP timeout。
      if(Date.now()<this.ticketFallbackUntil)return '';
      try{
        const controller=new AbortController();const timer=setTimeout(()=>controller.abort(),2200);
        const response=await fetch('/api/realtime-ticket',{
          method:'POST',headers:{'Authorization':`Bearer ${token}`,'Content-Type':'application/json'},body:'{}',cache:'no-store',signal:controller.signal
        }).finally(()=>clearTimeout(timer));
        if(!response.ok){this.ticketFallbackUntil=Date.now()+15000;return '';}
        const data=await response.json().catch(()=>({}));
        const ticket=String(data.ticket||'');
        if(ticket)this.ticketFallbackUntil=0;
        else this.ticketFallbackUntil=Date.now()+15000;
        return ticket;
      }catch(error){
        this.ticketFallbackUntil=Date.now()+30000;
        console.warn('建立即時連線票證失敗，30 秒內直接使用相容模式',error);return '';
      }
    }

    clearConnectTimer(){clearTimeout(this.connectTimer);this.connectTimer=null;}
    stopHeartbeat(){clearInterval(this.heartbeatTimer);this.heartbeatTimer=null;}
    startHeartbeat(){
      this.stopHeartbeat();
      this.heartbeatTimer=setInterval(()=>{
        if(this.closedByUser||!navigator.onLine)return;
        if(!this.socket||this.socket.readyState!==WebSocket.OPEN){this.reconnect();return;}
        const idle=Date.now()-this.lastActivityAt;
        if(idle>60000){
          try{this.socket.close(4001,'heartbeat timeout');}catch(_){}
          return;
        }
        this.lastPingAt=Date.now();
        this.sendPacket({event:'client:ping',data:{seq:++this.pingSeq,client_ts:this.lastPingAt}},false);
      },20000);
    }

    async open(){
      if(this.closedByUser||this.opening||!navigator.onLine)return;
      if(this.socket&&(this.socket.readyState===WebSocket.OPEN||this.socket.readyState===WebSocket.CONNECTING))return;
      this.opening=true;
      const generation=++this.openGeneration;
      try{
        const scheme=location.protocol==='https:'?'wss:':'ws:';
        const token=String(this.options?.auth?.token||'');
        const ticket=await this.createTicket();
        if(this.closedByUser||generation!==this.openGeneration)return;
        const authQuery=ticket?`ticket=${encodeURIComponent(ticket)}`:`token=${encodeURIComponent(token)}`;
        const url=`${scheme}//${location.host}/ws?${authQuery}`;
        let socket;
        try{socket=new WebSocket(url);}catch(error){this.scheduleReconnect();return;}
        this.socket=socket;
        this.clearConnectTimer();
        this.connectTimer=setTimeout(()=>{
          if(this.socket===socket&&socket.readyState===WebSocket.CONNECTING){
            try{socket.close();}catch(_){}
            this.socket=null;this.connected=false;this.scheduleReconnect();
          }
        },10000);
        socket.addEventListener('open',()=>{
          if(this.socket!==socket||generation!==this.openGeneration)return;
          this.clearConnectTimer();
          this.connected=true;this.retry=0;this.lastActivityAt=Date.now();this.startHeartbeat();
          // connect handler 會依目前 state.room 先送最新 room:enter。
          // 斷線期間排隊的舊 room:enter 必須丟棄，否則會重複觸發進房規則與完整快照。
          const queued=this.queue.splice(0);
          const replay=queued.filter(packet=>packet?.event!=='room:enter');
          this.dispatch('connect',{queued:replay.length,droppedRoomEnter:queued.length-replay.length});
          for(const packet of replay)this.sendPacket(packet);
        });
        socket.addEventListener('message',event=>{
          this.lastActivityAt=Date.now();
          try{
            const packet=JSON.parse(event.data);
            if(packet?.event==='server:pong'){
              const sent=Number(packet?.data?.client_ts)||this.lastPingAt;
              this.dispatch('latency',{ms:Math.max(0,Date.now()-sent),seq:packet?.data?.seq});
              return;
            }
            if(packet?.event&&packet.event!=='connect')this.dispatch(packet.event,packet.data);
          }catch(error){console.warn('忽略無法解析的即時訊息',error);}
        });
        socket.addEventListener('close',event=>{
          if(this.socket!==socket)return;
          this.clearConnectTimer();this.stopHeartbeat();
          this.socket=null;this.connected=false;
          if(!this.closedByUser){
            this.dispatch('disconnect',{code:event.code,reason:event.reason||''});
            this.dispatch('connect_error',{code:event.code,reason:event.reason||''});
            this.scheduleReconnect();
          }
        });
        socket.addEventListener('error',()=>{});
      }finally{if(generation===this.openGeneration)this.opening=false;}
    }

    sendPacket(packet,queueOnFail=true){
      if(this.socket?.readyState===WebSocket.OPEN){
        try{this.socket.send(JSON.stringify(packet));return true;}catch(_){}
      }
      if(queueOnFail)this.enqueue(packet);
      return false;
    }

    enqueue(packet){
      // room:enter 只保留最後一個，避免斷線期間切房後重播過期進房事件。
      if(packet?.event==='room:enter')this.queue=this.queue.filter(item=>item?.event!=='room:enter');
      if(packet?.event==='client:ping')return;
      if(this.queue.length>=50)this.queue.shift();
      this.queue.push(packet);
    }

    emit(event,data){
      const packet={event,data:data??{}};
      this.sendPacket(packet,true);
      return this;
    }

    scheduleReconnect(){
      if(this.closedByUser||this.retryTimer||!navigator.onLine)return;
      const base=Math.min(10000,300*(2**Math.min(this.retry,5)));
      const delay=Math.round(base*(0.85+Math.random()*0.3));
      this.retry+=1;
      this.dispatch('reconnecting',{attempt:this.retry,delay});
      this.retryTimer=setTimeout(()=>{this.retryTimer=null;this.open();},delay);
    }

    reconnect(){
      if(this.closedByUser||!navigator.onLine)return this;
      ++this.openGeneration;this.opening=false;
      clearTimeout(this.retryTimer);this.retryTimer=null;this.clearConnectTimer();this.stopHeartbeat();
      const old=this.socket;this.socket=null;this.connected=false;
      if(old&&old.readyState<2){try{old.close(4000,'reconnect');}catch(_){}}
      this.open();return this;
    }

    disconnect(){
      this.closedByUser=true;this.connected=false;++this.openGeneration;this.opening=false;clearTimeout(this.retryTimer);this.retryTimer=null;this.clearConnectTimer();this.stopHeartbeat();this.queue=[];
      window.removeEventListener('online',this.onlineHandler);window.removeEventListener('offline',this.offlineHandler);document.removeEventListener('visibilitychange',this.visibilityHandler);
      if(this.socket&&this.socket.readyState<2)this.socket.close(1000,'client disconnect');
      this.socket=null;
    }
  }

  window.io=options=>new NativeSocket(options);
})();
