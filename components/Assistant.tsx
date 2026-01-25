
import React, { useState, useRef, useEffect } from 'react';
import { chatAssistant } from '../services/geminiService';

interface AssistantProps {
  inventory: any[];
}

const Assistant: React.FC<AssistantProps> = ({ inventory }) => {
  const [isOpen, setIsOpen] = useState(false);
  const [messages, setMessages] = useState<{ role: 'user' | 'bot'; text: string }[]>([
    { role: 'bot', text: 'Ciao! Come posso aiutarti oggi in cucina?' }
  ]);
  const [inputValue, setInputValue] = useState('');
  const chatRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (chatRef.current) {
      chatRef.current.scrollTop = chatRef.current.scrollHeight;
    }
  }, [messages]);

  const handleSend = async () => {
    if (!inputValue.trim()) return;
    const userMsg = inputValue;
    setMessages(prev => [...prev, { role: 'user', text: userMsg }]);
    setInputValue('');
    
    try {
      const botResponse = await chatAssistant(userMsg, inventory);
      setMessages(prev => [...prev, { role: 'bot', text: botResponse || "Non ho capito, scusa." }]);
    } catch (err) {
      setMessages(prev => [...prev, { role: 'bot', text: "Errore nel contattare l'assistente." }]);
    }
  };

  return (
    <>
      <div className="fixed bottom-24 right-6 z-50">
        <button 
          onClick={() => setIsOpen(!isOpen)}
          className={`w-14 h-14 rounded-full shadow-lg flex items-center justify-center transition-all ${isOpen ? 'bg-rose-500 rotate-45' : 'bg-emerald-500 hover:scale-110'}`}
        >
          {isOpen ? (
            <svg className="w-6 h-6 text-white" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M12 4v16m8-8H4" /></svg>
          ) : (
            <svg className="w-6 h-6 text-white" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M19 11a7 7 0 01-7 7m0 0a7 7 0 01-7-7m7 7v4m0 0H8m4 0h4m-4-8a3 3 0 01-3-3V5a3 3 0 116 0v6a3 3 0 01-3 3z" /></svg>
          )}
        </button>
      </div>

      {isOpen && (
        <div className="fixed bottom-40 right-6 left-6 md:left-auto md:w-96 glass rounded-3xl p-4 shadow-2xl z-50 animate-in fade-in slide-in-from-bottom-4">
          <div className="flex flex-col h-96">
            <div className="flex items-center gap-3 pb-3 border-b border-white/10 mb-3">
              <div className="w-8 h-8 rounded-full bg-emerald-500/20 flex items-center justify-center text-emerald-400">
                AI
              </div>
              <h3 className="font-bold text-lg">Assistente Fridge44</h3>
            </div>
            
            <div ref={chatRef} className="flex-1 overflow-y-auto space-y-4 pr-2 mb-4">
              {messages.map((m, i) => (
                <div key={i} className={`flex ${m.role === 'user' ? 'justify-end' : 'justify-start'}`}>
                  <div className={`max-w-[85%] px-4 py-2 rounded-2xl ${m.role === 'user' ? 'bg-emerald-600 text-white rounded-tr-none' : 'bg-white/5 text-gray-200 rounded-tl-none'}`}>
                    <p className="text-sm">{m.text}</p>
                  </div>
                </div>
              ))}
            </div>

            <div className="relative">
              <input 
                value={inputValue}
                onChange={(e) => setInputValue(e.target.value)}
                onKeyPress={(e) => e.key === 'Enter' && handleSend()}
                placeholder="Chiedimi qualcosa..."
                className="w-full bg-white/5 border border-white/10 rounded-xl py-3 px-4 focus:ring-2 focus:ring-emerald-500 outline-none"
              />
              <button 
                onClick={handleSend}
                className="absolute right-2 top-2 p-1.5 bg-emerald-500 rounded-lg"
              >
                <svg className="w-5 h-5 text-white" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path strokeLinecap="round" strokeLinejoin="round" strokeWidth="2" d="M13 5l7 7-7 7M5 5l7 7-7 7" /></svg>
              </button>
            </div>
          </div>
        </div>
      )}
    </>
  );
};

export default Assistant;
