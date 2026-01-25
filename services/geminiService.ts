
import { GoogleGenAI, Type } from "@google/genai";

const ai = new GoogleGenAI({ apiKey: process.env.API_KEY });

export const analyzeProductImage = async (base64Image: string) => {
  const response = await ai.models.generateContent({
    model: 'gemini-3-flash-preview',
    contents: {
      parts: [
        { inlineData: { data: base64Image, mimeType: 'image/jpeg' } },
        { text: "Agisci come Google Lens. Analizza l'immagine del prodotto alimentare con massima precisione. Estrai: nome esatto, marca, categoria (scegli tra [Latticini, Carne/Pesce, Verdura, Frutta, Secchi, Bevande, Cucinato, Altro]), data di scadenza (formato YYYY-MM-DD se visibile), quantità numerica e unità di misura. Se noti loghi di supermercati (es. Lidl, Coop, Carrefour) sulla confezione, identifica il possibile shopId. Fornisci un flag 'expiryFoundInImage' (true se letta, false se stimata). Restituisci JSON in italiano." }
      ]
    },
    config: {
      responseMimeType: "application/json",
      responseSchema: {
        type: Type.OBJECT,
        properties: {
          name: { type: Type.STRING },
          brand: { type: Type.STRING },
          category: { type: Type.STRING },
          expiryDate: { type: Type.STRING },
          quantity: { type: Type.NUMBER },
          unit: { type: Type.STRING },
          expiryFoundInImage: { type: Type.BOOLEAN },
          suggestedShopName: { type: Type.STRING }
        }
      }
    }
  });
  return JSON.parse(response.text || '{}');
};

export const analyzeReceipt = async (base64Image: string) => {
  const response = await ai.models.generateContent({
    model: 'gemini-3-flash-preview',
    contents: {
      parts: [
        { inlineData: { data: base64Image, mimeType: 'image/jpeg' } },
        { text: "Estrai info scontrino: storeName (nome negozio) e items (nome articolo, quantità, prezzoUnitario). Restituisci JSON in italiano." }
      ]
    },
    config: {
      responseMimeType: "application/json",
      responseSchema: {
        type: Type.OBJECT,
        properties: {
          storeName: { type: Type.STRING },
          items: {
            type: Type.ARRAY,
            items: {
              type: Type.OBJECT,
              properties: {
                name: { type: Type.STRING },
                quantity: { type: Type.NUMBER },
                pricePerUnit: { type: Type.NUMBER }
              }
            }
          }
        }
      }
    }
  });
  return JSON.parse(response.text || '{}');
};

export const parseVoiceCommand = async (transcript: string) => {
  const response = await ai.models.generateContent({
    model: 'gemini-3-flash-preview',
    contents: `Analizza questo comando vocale per l'inventario cibo: "${transcript}". Identifica azione (aggiungi, rimuovi, modifica), prodotto, quantità e posizione (Frigo, Freezer o Dispensa). Restituisci JSON in italiano.`,
    config: {
      responseMimeType: "application/json",
      responseSchema: {
        type: Type.OBJECT,
        properties: {
          action: { type: Type.STRING, description: "aggiungi, rimuovi o modifica" },
          product: { type: Type.STRING },
          quantity: { type: Type.NUMBER },
          location: { type: Type.STRING, description: "Frigo, Freezer o Dispensa" },
          unit: { type: Type.STRING }
        }
      }
    }
  });
  return JSON.parse(response.text || '{}');
};

export const generateRecipeSuggestions = async (selectedIngredients: string[], allInventory: string[]) => {
  const response = await ai.models.generateContent({
    model: 'gemini-3-flash-preview',
    contents: `Basandoti su questi ingredienti selezionati: ${selectedIngredients.join(", ")}, suggerisci 3 ricette salutari in LINGUA ITALIANA. 
    Controlla se gli altri ingredienti necessari sono presenti nell'inventario completo: ${allInventory.join(", ")}.
    Per ogni ricetta, elenca gli ingredienti mancanti. IMPORTANTE: Le istruzioni devono essere divise in passaggi chiari con un'icona emoji all'inizio di ogni passaggio. Restituisci JSON array.`,
    config: {
      responseMimeType: "application/json",
      responseSchema: {
        type: Type.ARRAY,
        items: {
          type: Type.OBJECT,
          properties: {
            title: { type: Type.STRING },
            description: { type: Type.STRING },
            ingredients: { type: Type.ARRAY, items: { type: Type.STRING } },
            missingIngredients: { type: Type.ARRAY, items: { type: Type.STRING } },
            difficulty: { type: Type.STRING },
            time: { type: Type.NUMBER },
            instructions: { type: Type.STRING }
          }
        }
      }
    }
  });
  return JSON.parse(response.text || '[]');
};

export const searchWebRecipes = async (ingredients: string[]) => {
  const response = await ai.models.generateContent({
    model: 'gemini-3-flash-preview',
    contents: `Cerca ricette reali sul web o YouTube per: ${ingredients.join(", ")}. 
    Fornisci titoli, descrizioni e YouTube ID reali se possibile in LINGUA ITALIANA. 
    Dividi le istruzioni in passaggi con emoji. Restituisci JSON array.`,
    config: {
      responseMimeType: "application/json",
      responseSchema: {
        type: Type.ARRAY,
        items: {
          type: Type.OBJECT,
          properties: {
            title: { type: Type.STRING },
            description: { type: Type.STRING },
            ingredients: { type: Type.ARRAY, items: { type: Type.STRING } },
            missingIngredients: { type: Type.ARRAY, items: { type: Type.STRING } },
            difficulty: { type: Type.STRING },
            time: { type: Type.NUMBER },
            youtubeId: { type: Type.STRING },
            url: { type: Type.STRING },
            instructions: { type: Type.STRING }
          }
        }
      }
    }
  });
  return JSON.parse(response.text || '[]');
};

export const chatAssistant = async (message: string, context: any) => {
  const response = await ai.models.generateContent({
    model: 'gemini-3-flash-preview',
    contents: message,
    config: {
      systemInstruction: `Sei l'assistente AI di Fridge44. Aiuta a gestire lo stock: ${JSON.stringify(context)}. Sii conciso, utile e amichevole. Parla ESCLUSIVAMENTE in italiano.`
    }
  });
  return response.text;
};
